"""Polling mode (non-event) loop for the RS-485 main host."""

from __future__ import annotations

import time

from photon_rs485 import (
    FRAME_TYPE_DATA_REQ,
    FRAME_TYPE_DATA_RESP,
)
from app.utils import read_serial_input, safe_print, serial_poll_requested, toggle_usb_logging

from .calibration import handle_cal_reset, handle_cal_save
from .console import print_full_table, print_table
from .constants import (
    DATA_RESPONSE_TIMEOUT_S,
    HEALTH_CHECK_IDLE_S,
    MAX_SENSORS,
    PING_INTERVAL_S,
    PRINT_INTERVAL_S,
    REQUEST_INTERVAL_S,
    SENSOR_NODE_DEVICE_IDS,
    SENSOR_VALUE_MAX,
    TOTAL_SENSORS,
)
from .midi_mapping import board_index
from .protocol import decode_values, health_check, ping_all_sequential, poll_all_sensor_nodes, poll_minmax_for_sensor_node, print_health_results


def run_polling_mode(
    bus,
    *,
    min_range: int,
    index_to_midi,
    logical_index,
) -> None:
    next_request = time.monotonic() + REQUEST_INTERVAL_S
    next_print = time.monotonic() + PRINT_INTERVAL_S
    req_seq = 0
    request_index = 0
    ping_seq = 0
    last_data_rx = {sensor_device_id: None for sensor_device_id in SENSOR_NODE_DEVICE_IDS}
    sensor_values = [0] * TOTAL_SENSORS
    sensor_values_seen = [False] * TOTAL_SENSORS
    sensor_stds = [0] * TOTAL_SENSORS
    sensor_std_seen = [False] * TOTAL_SENSORS
    sensor_mins = [SENSOR_VALUE_MAX] * TOTAL_SENSORS
    sensor_maxs = [0] * TOTAL_SENSORS
    sensor_minmax_seen = [False] * TOTAL_SENSORS
    req_sent = {sensor_device_id: 0 for sensor_device_id in SENSOR_NODE_DEVICE_IDS}
    req_ok = {sensor_device_id: 0 for sensor_device_id in SENSOR_NODE_DEVICE_IDS}
    req_drop = {sensor_device_id: 0 for sensor_device_id in SENSOR_NODE_DEVICE_IDS}
    req_last_print = {sensor_device_id: 0 for sensor_device_id in SENSOR_NODE_DEVICE_IDS}
    ping_ok = {sensor_device_id: 0 for sensor_device_id in SENSOR_NODE_DEVICE_IDS}
    ping_sent = {sensor_device_id: 0 for sensor_device_id in SENSOR_NODE_DEVICE_IDS}
    next_ping = time.monotonic() + PING_INTERVAL_S
    data_in_flight = None
    data_deadline = 0.0
    stats_seq = 0
    next_health_check = time.monotonic() + HEALTH_CHECK_IDLE_S
    health_seen = False
    health_warned = False
    cal_seq_ref = [0]

    def _handle_frame(frame_type, target_id, source_id, payload, seq, now) -> None:
        nonlocal data_in_flight
        if frame_type == FRAME_TYPE_DATA_RESP:
            values, stds = decode_values(payload)
            last_data_rx[source_id] = now
            if data_in_flight == source_id:
                data_in_flight = None
            if source_id in req_ok:
                req_ok[source_id] += 1
            board_idx = board_index(source_id)
            if board_idx is None:
                return
            base = board_idx * MAX_SENSORS
            for idx, value in enumerate(values):
                sensor_idx = base + idx
                if 0 <= sensor_idx < TOTAL_SENSORS:
                    sensor_values[sensor_idx] = value
                    sensor_values_seen[sensor_idx] = True
                    if idx < len(stds):
                        sensor_stds[sensor_idx] = stds[idx]
                        sensor_std_seen[sensor_idx] = True

    def _drain_frames(now) -> None:
        for frame_type, target_id, source_id, payload, seq in bus.read_frames():
            _handle_frame(frame_type, target_id, source_id, payload, seq, now)

    def _do_health_check() -> None:
        nonlocal stats_seq, health_seen, health_warned
        stats_responses, stats_seq = health_check(bus, stats_seq, on_frame=_handle_frame)
        health_seen, health_warned = print_health_results(
            stats_responses, health_seen=health_seen, health_warned=health_warned
        )

    _do_health_check()
    next_health_check = time.monotonic() + HEALTH_CHECK_IDLE_S
    while True:
        now = time.monotonic()
        if data_in_flight is None and now >= next_request:
            target_id = SENSOR_NODE_DEVICE_IDS[request_index % len(SENSOR_NODE_DEVICE_IDS)]
            bus.send_frame(FRAME_TYPE_DATA_REQ, target_id, b"", req_seq & 0xFFFF, ack_timeout_us=0)
            req_seq += 1
            if target_id in req_sent:
                req_sent[target_id] += 1
            data_in_flight = target_id
            data_deadline = now + DATA_RESPONSE_TIMEOUT_S
            request_index = (request_index + 1) % len(SENSOR_NODE_DEVICE_IDS)
            next_request = now + REQUEST_INTERVAL_S

        if now >= next_ping:
            ping_seq, pong_received = ping_all_sequential(
                bus, SENSOR_NODE_DEVICE_IDS, ping_seq, on_frame=_handle_frame,
            )
            for sensor_device_id in SENSOR_NODE_DEVICE_IDS:
                ping_sent[sensor_device_id] = ping_sent.get(sensor_device_id, 0) + 1
                if sensor_device_id in pong_received:
                    ping_ok[sensor_device_id] = ping_ok.get(sensor_device_id, 0) + 1
            next_ping = now + PING_INTERVAL_S

        if data_in_flight is not None and now >= data_deadline:
            if data_in_flight in req_drop:
                req_drop[data_in_flight] += 1
            data_in_flight = None

        _drain_frames(now)

        if serial_poll_requested():
            data = read_serial_input()
            for ch in data:
                if ch in ("\n", "\r"):
                    data_in_flight = None
                    data_deadline = 0.0
                    poll_all_sensor_nodes(
                        bus,
                        sensor_values,
                        sensor_values_seen,
                        sensor_stds,
                        sensor_std_seen,
                        sensor_mins,
                        sensor_maxs,
                        sensor_minmax_seen,
                        timeout_s=DATA_RESPONSE_TIMEOUT_S,
                    )
                    print_full_table(
                        sensor_values,
                        sensor_values_seen,
                        sensor_stds,
                        sensor_std_seen,
                        sensor_mins,
                        sensor_maxs,
                        sensor_minmax_seen,
                        index_to_midi,
                        logical_index,
                        min_range,
                    )
                elif ch in ("s", "S"):
                    handle_cal_save(bus, cal_seq_ref)
                elif ch in ("r", "R"):
                    handle_cal_reset(bus, cal_seq_ref)
                elif ch in ("p", "P"):
                    toggle_usb_logging()

        if now >= next_health_check:
            _do_health_check()
            next_health_check = now + HEALTH_CHECK_IDLE_S

        if now >= next_print:
            for sensor_device_id in SENSOR_NODE_DEVICE_IDS:
                poll_minmax_for_sensor_node(
                    bus,
                    sensor_device_id,
                    sensor_mins,
                    sensor_maxs,
                    sensor_minmax_seen,
                    sensor_stds,
                    sensor_std_seen,
                    timeout_s=DATA_RESPONSE_TIMEOUT_S,
                )
                print_table(
                    sensor_device_id,
                    sensor_values,
                    sensor_values_seen,
                    sensor_stds,
                    sensor_std_seen,
                    sensor_mins,
                    sensor_maxs,
                    sensor_minmax_seen,
                    index_to_midi,
                    min_range,
                )
                sent = req_sent.get(sensor_device_id, 0)
                ok = req_ok.get(sensor_device_id, 0)
                rate = (ok / sent * 100.0) if sent else 0.0
                dropped = req_drop.get(sensor_device_id, 0)
                elapsed = max(now - req_last_print.get(sensor_device_id, now), 0.001)
                poll_rate = sent / elapsed
                safe_print(
                    f"# reqs sensor node {sensor_device_id}: drop={dropped} ok={ok} sent={sent} ({rate:.1f}%) poll={poll_rate:.1f}/s"
                )
                ping_ok_count = ping_ok.get(sensor_device_id, 0)
                ping_sent_count = ping_sent.get(sensor_device_id, 0)
                safe_print(f"# ping ok sensor node {sensor_device_id}: {ping_ok_count}/{ping_sent_count}")
                req_sent[sensor_device_id] = 0
                req_ok[sensor_device_id] = 0
                req_drop[sensor_device_id] = 0
                ping_ok[sensor_device_id] = 0
                ping_sent[sensor_device_id] = 0
                req_last_print[sensor_device_id] = now
            next_print = now + PRINT_INTERVAL_S
