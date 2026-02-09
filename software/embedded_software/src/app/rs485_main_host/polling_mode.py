"""Polling mode (non-event) loop for the RS-485 main host."""

from __future__ import annotations

import time

from app.rs485_bus import (
    FRAME_TYPE_DATA_REQ,
    FRAME_TYPE_DATA_RESP,
    FRAME_TYPE_EVENT,
    FRAME_TYPE_PING,
    FRAME_TYPE_PONG,
    FRAME_TYPE_STATS_REQ,
    FRAME_TYPE_STATS_RESP,
)
from app.rs485_common.serial import read_serial_input, serial_poll_requested

from .calibration import handle_cal_reset, handle_cal_save
from .console import print_full_table, print_table
from .constants import (
    DATA_RESPONSE_TIMEOUT_S,
    DATA_TIMEOUT_S,
    DISPLAY_UPDATE_S,
    HEALTH_CHECK_IDLE_S,
    MAX_SENSORS,
    PING_INTERVAL_S,
    PRINT_INTERVAL_S,
    REQUEST_INTERVAL_S,
    SENSOR_NODE_DEVICE_IDS,
    SENSOR_VALUE_MAX,
    STATS_RESPONSE_TIMEOUT_S,
    TOTAL_SENSORS,
)
from .protocol import decode_values, poll_all_sensor_nodes, poll_minmax_for_sensor_node, send_with_gap


def run_polling_mode(
    bus,
    *,
    min_range: int,
    index_to_midi,
    logical_index,
    display_enabled: bool,
    update_display=None,
) -> None:
    next_request = time.monotonic() + REQUEST_INTERVAL_S
    next_print = time.monotonic() + PRINT_INTERVAL_S
    next_display = time.monotonic() + DISPLAY_UPDATE_S
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
    ping_pending_seq = {sensor_device_id: None for sensor_device_id in SENSOR_NODE_DEVICE_IDS}
    ping_deadline = {sensor_device_id: 0.0 for sensor_device_id in SENSOR_NODE_DEVICE_IDS}
    next_ping = time.monotonic() + PING_INTERVAL_S
    data_in_flight = None
    data_deadline = 0.0
    stats_seq = 0
    next_health_check = time.monotonic() + HEALTH_CHECK_IDLE_S
    health_seen = False
    health_warned = False
    cal_seq_ref = [0]

    def _handle_frame(frame_type, target_id, payload, seq, now) -> None:
        nonlocal data_in_flight, next_display
        if frame_type == FRAME_TYPE_PONG:
            pending_seq = ping_pending_seq.get(target_id)
            if pending_seq is not None and seq == pending_seq:
                if now <= ping_deadline.get(target_id, 0.0):
                    ping_ok[target_id] = ping_ok.get(target_id, 0) + 1
                ping_pending_seq[target_id] = None
        elif frame_type == FRAME_TYPE_DATA_RESP:
            values, stds = decode_values(payload)
            last_data_rx[target_id] = now
            if data_in_flight == target_id:
                data_in_flight = None
            if target_id in req_ok:
                req_ok[target_id] += 1
            base = (target_id - 1) * MAX_SENSORS
            for idx, value in enumerate(values):
                sensor_idx = base + idx
                if 0 <= sensor_idx < TOTAL_SENSORS:
                    sensor_values[sensor_idx] = value
                    sensor_values_seen[sensor_idx] = True
                    if idx < len(stds):
                        sensor_stds[sensor_idx] = stds[idx]
                        sensor_std_seen[sensor_idx] = True
            if display_enabled and update_display is not None and now >= next_display:
                update_display(sensor_values)
                next_display = now + DISPLAY_UPDATE_S
        elif frame_type == FRAME_TYPE_STATS_RESP:
            pass
        elif frame_type == FRAME_TYPE_EVENT:
            pass

    def _drain_frames(now) -> None:
        for frame_type, target_id, payload, seq in bus.read_frames():
            _handle_frame(frame_type, target_id, payload, seq, now)

    def _collect_stats(seq: int, timeout_s: float):
        nonlocal data_in_flight
        responses = {}
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            for frame_type, rx_target_id, payload, rx_seq in bus.read_frames():
                if frame_type == FRAME_TYPE_STATS_RESP and rx_seq == seq:
                    last_data_rx[rx_target_id] = time.monotonic()
                    rate = None
                    if len(payload) >= 2:
                        rate_tenths = int.from_bytes(payload[:2], "little")
                        rate = rate_tenths / 10.0
                    evt_tx = evt_drop = evt_retry = None
                    if len(payload) >= 8:
                        evt_tx = int.from_bytes(payload[2:4], "little")
                        evt_drop = int.from_bytes(payload[4:6], "little")
                        evt_retry = int.from_bytes(payload[6:8], "little")
                    responses[rx_target_id] = (rate, evt_tx, evt_drop, evt_retry)
                    if len(responses) >= len(SENSOR_NODE_DEVICE_IDS):
                        return responses
                    continue
                _handle_frame(frame_type, rx_target_id, payload, rx_seq, time.monotonic())
            if data_in_flight is not None and time.monotonic() >= data_deadline:
                if data_in_flight in req_drop:
                    req_drop[data_in_flight] += 1
                data_in_flight = None
            time.sleep(0.0005)
        return responses

    def _health_check() -> None:
        nonlocal stats_seq, health_seen, health_warned
        seq = stats_seq & 0xFFFF
        stats_seq = (stats_seq + 1) & 0xFFFF
        bus.send_frame(FRAME_TYPE_STATS_REQ, 0, b"", seq)
        stats_responses = _collect_stats(seq, STATS_RESPONSE_TIMEOUT_S)
        if stats_responses:
            health_seen = True
        elif not health_seen and not health_warned:
            print(
                "\n!!! RS485 HEALTH CHECK FAILED: no responses from any sensor node !!!\n"
                "    Check power, wiring, device_id, and baud rate.\n"
            )
            health_warned = True
        for sensor_device_id in SENSOR_NODE_DEVICE_IDS:
            stats = stats_responses.get(sensor_device_id)
            if stats is None:
                print(f"# status sensor node {sensor_device_id}: no response")
                continue
            rate, evt_tx, evt_drop, evt_retry = stats
            rate_val = rate if rate is not None else 0.0
            if evt_tx is None:
                print(f"# status sensor node {sensor_device_id}: alive rate={rate_val:.1f} Hz")
            else:
                print(
                    f"# status sensor node {sensor_device_id}: alive rate={rate_val:.1f} Hz "
                    f"evt_tx={evt_tx} evt_drop={evt_drop} evt_retry={evt_retry}"
                )

    _health_check()
    next_health_check = time.monotonic() + HEALTH_CHECK_IDLE_S
    while True:
        now = time.monotonic()
        if data_in_flight is None and now >= next_request:
            target_id = SENSOR_NODE_DEVICE_IDS[request_index % len(SENSOR_NODE_DEVICE_IDS)]
            send_with_gap(bus, FRAME_TYPE_DATA_REQ, target_id, b"", req_seq & 0xFFFF)
            req_seq += 1
            if target_id in req_sent:
                req_sent[target_id] += 1
            data_in_flight = target_id
            data_deadline = now + DATA_RESPONSE_TIMEOUT_S
            request_index = (request_index + 1) % len(SENSOR_NODE_DEVICE_IDS)
            next_request = now + REQUEST_INTERVAL_S

        if now >= next_ping:
            for sensor_device_id in SENSOR_NODE_DEVICE_IDS:
                seq = ping_seq & 0xFFFF
                send_with_gap(bus, FRAME_TYPE_PING, sensor_device_id, b"", seq)
                ping_pending_seq[sensor_device_id] = seq
                ping_deadline[sensor_device_id] = now + DATA_RESPONSE_TIMEOUT_S
                ping_sent[sensor_device_id] = ping_sent.get(sensor_device_id, 0) + 1
                ping_seq += 1
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

        if now >= next_health_check:
            _health_check()
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
                print(
                    f"# reqs sensor node {sensor_device_id}: drop={dropped} ok={ok} sent={sent} ({rate:.1f}%) poll={poll_rate:.1f}/s"
                )
                ping_ok_count = ping_ok.get(sensor_device_id, 0)
                ping_sent_count = ping_sent.get(sensor_device_id, 0)
                print(f"# ping ok sensor node {sensor_device_id}: {ping_ok_count}/{ping_sent_count}")
                req_sent[sensor_device_id] = 0
                req_ok[sensor_device_id] = 0
                req_drop[sensor_device_id] = 0
                ping_ok[sensor_device_id] = 0
                ping_sent[sensor_device_id] = 0
                req_last_print[sensor_device_id] = now
            next_print = now + PRINT_INTERVAL_S
