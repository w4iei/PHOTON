"""RS-485 protocol helpers for the main host."""

from __future__ import annotations

import time

from app.rs485_bus import (
    FRAME_TYPE_DATA_REQ,
    FRAME_TYPE_DATA_RESP,
    FRAME_TYPE_EVENT,
    FRAME_TYPE_MINMAX_REQ,
    FRAME_TYPE_MINMAX_RESP,
    FRAME_TYPE_TRACE_REQ,
    FRAME_TYPE_TRACE_RESP,
)

from .constants import (
    DATA_RECORD_BYTES,
    DATA_RESPONSE_TIMEOUT_S,
    INTER_FRAME_GAP_S,
    MAX_SENSORS,
    MINMAX_RECORD_BYTES,
    REQUEST_INTERVAL_S,
    SENSOR_NODE_DEVICE_IDS,
    SENSOR_VALUE_MAX,
    TOTAL_SENSORS,
)
from .midi_mapping import board_index


def decode_values(payload: bytes):
    if len(payload) % DATA_RECORD_BYTES == 0:
        values = []
        stds = []
        for i in range(0, len(payload), DATA_RECORD_BYTES):
            values.append(int.from_bytes(payload[i : i + 2], "little"))
            stds.append(int.from_bytes(payload[i + 2 : i + 4], "little"))
        return values, stds
    if len(payload) % 2:
        return [], []
    values = []
    for i in range(0, len(payload), 2):
        values.append(int.from_bytes(payload[i : i + 2], "little"))
    return values, [0] * len(values)


def apply_values(
    board_id: int,
    values,
    stds,
    sensor_values,
    sensor_stds,
    sensor_values_seen,
    sensor_std_seen,
) -> None:
    base = (board_id - 1) * MAX_SENSORS
    for idx, value in enumerate(values):
        sensor_idx = base + idx
        if 0 <= sensor_idx < TOTAL_SENSORS:
            sensor_values[sensor_idx] = value
            sensor_values_seen[sensor_idx] = True
            if idx < len(stds):
                sensor_stds[sensor_idx] = stds[idx]
                sensor_std_seen[sensor_idx] = True


def apply_minmax_payload_local(payload: bytes, mins, maxs, seen) -> int:
    if len(payload) < 2:
        return 0
    start = payload[0]
    count = payload[1]
    available = max((len(payload) - 2) // MINMAX_RECORD_BYTES, 0)
    count = min(count, available)
    new_seen = 0
    for idx in range(count):
        sensor_idx = start + idx
        if sensor_idx >= len(mins):
            break
        offset = 2 + (idx * MINMAX_RECORD_BYTES)
        min_v = int.from_bytes(payload[offset : offset + 2], "little")
        max_v = int.from_bytes(payload[offset + 2 : offset + 4], "little")
        mins[sensor_idx] = min_v
        maxs[sensor_idx] = max_v
        if not seen[sensor_idx]:
            seen[sensor_idx] = True
            new_seen += 1
    return new_seen


def send_with_gap(bus, frame_type: int, target_id: int, payload: bytes, seq: int) -> None:
    bus.send_frame(frame_type, target_id, payload, seq)
    time.sleep(INTER_FRAME_GAP_S)


def poll_all_sensor_nodes(
    bus,
    sensor_values,
    sensor_values_seen,
    sensor_stds,
    sensor_std_seen,
    sensor_mins,
    sensor_maxs,
    sensor_minmax_seen,
    *,
    timeout_s: float,
    on_event=None,
) -> None:
    seq = 0
    for board_id in SENSOR_NODE_DEVICE_IDS:
        bus.send_frame(FRAME_TYPE_DATA_REQ, board_id, b"", seq & 0xFFFF)
        seq = (seq + 1) & 0xFFFF
        deadline = time.monotonic() + timeout_s
        received = False
        while time.monotonic() < deadline:
            for frame_type, target_id, payload, rx_seq in bus.read_frames():
                if frame_type == FRAME_TYPE_DATA_RESP and target_id == board_id:
                    values, stds = decode_values(payload)
                    apply_values(
                        board_id,
                        values,
                        stds,
                        sensor_values,
                        sensor_stds,
                        sensor_values_seen,
                        sensor_std_seen,
                    )
                    received = True
                    break
                if frame_type == FRAME_TYPE_EVENT and on_event is not None:
                    on_event(frame_type, target_id, payload, rx_seq, time.monotonic())
            if received:
                break
            time.sleep(0.0005)
        if received:
            poll_minmax_for_sensor_node(
                bus,
                board_id,
                sensor_mins,
                sensor_maxs,
                sensor_minmax_seen,
                sensor_stds,
                sensor_std_seen,
                timeout_s=timeout_s,
                on_event=on_event,
            )


def poll_minmax_for_sensor_node(
    bus,
    board_id: int,
    mins,
    maxs,
    seen,
    stds,
    std_seen,
    *,
    timeout_s: float,
    on_event=None,
) -> None:
    bus_max_payload = getattr(bus, "max_payload", MAX_SENSORS * 6)
    max_count = max((bus_max_payload - 2) // MINMAX_RECORD_BYTES, 0)
    if max_count <= 0:
        return
    base = (board_id - 1) * MAX_SENSORS
    start = 0
    seq = 0
    while start < MAX_SENSORS:
        count = min(max_count, MAX_SENSORS - start)
        payload = bytes([start & 0xFF, count & 0xFF])
        bus.send_frame(FRAME_TYPE_MINMAX_REQ, board_id, payload, seq & 0xFFFF)
        seq = (seq + 1) & 0xFFFF
        deadline = time.monotonic() + timeout_s
        received = False
        resp_count = 0
        while time.monotonic() < deadline:
            for frame_type, target_id, payload, rx_seq in bus.read_frames():
                if frame_type == FRAME_TYPE_MINMAX_RESP and target_id == board_id:
                    if len(payload) >= 2:
                        resp_count = payload[1]
                    if resp_count <= 0:
                        return
                    resp_start = payload[0]
                    available = max((len(payload) - 2) // MINMAX_RECORD_BYTES, 0)
                    resp_count = min(resp_count, available)
                    for idx in range(resp_count):
                        offset = 2 + (idx * MINMAX_RECORD_BYTES)
                        min_v = int.from_bytes(payload[offset : offset + 2], "little")
                        max_v = int.from_bytes(payload[offset + 2 : offset + 4], "little")
                        global_idx = base + resp_start + idx
                        if 0 <= global_idx < len(mins):
                            mins[global_idx] = min_v
                            maxs[global_idx] = max_v
                            seen[global_idx] = True
                            if offset + 4 + 2 <= len(payload):
                                std_v = int.from_bytes(payload[offset + 4 : offset + 6], "little")
                                stds[global_idx] = std_v
                                std_seen[global_idx] = True
                    received = True
                    break
                if frame_type == FRAME_TYPE_EVENT and on_event is not None:
                    on_event(frame_type, target_id, payload, rx_seq, time.monotonic())
            if received:
                break
            time.sleep(0.0005)
        if not received:
            return
        if resp_count < count:
            return
        start += count


def calibrate_min_max(bus, duration_s: float):
    mins = [SENSOR_VALUE_MAX] * TOTAL_SENSORS
    maxs = [0] * TOTAL_SENSORS
    seen = [False] * TOTAL_SENSORS
    next_request = time.monotonic()
    req_seq = 0
    request_index = 0
    data_in_flight = None
    data_deadline = 0.0
    deadline = time.monotonic() + duration_s
    while time.monotonic() < deadline:
        now = time.monotonic()
        if data_in_flight is None and now >= next_request:
            target_id = SENSOR_NODE_DEVICE_IDS[request_index % len(SENSOR_NODE_DEVICE_IDS)]
            send_with_gap(bus, FRAME_TYPE_DATA_REQ, target_id, b"", req_seq & 0xFFFF)
            req_seq += 1
            data_in_flight = target_id
            data_deadline = now + DATA_RESPONSE_TIMEOUT_S
            request_index = (request_index + 1) % len(SENSOR_NODE_DEVICE_IDS)
            next_request = now + REQUEST_INTERVAL_S

        if data_in_flight is not None and now >= data_deadline:
            data_in_flight = None

        for frame_type, target_id, payload, seq in bus.read_frames():
            if frame_type != FRAME_TYPE_DATA_RESP:
                continue
            values, _stds = decode_values(payload)
            board_idx = board_index(target_id)
            if board_idx is None:
                continue
            base = board_idx * MAX_SENSORS
            for idx, value in enumerate(values):
                sensor_idx = base + idx
                if sensor_idx >= TOTAL_SENSORS:
                    break
                if not seen[sensor_idx]:
                    seen[sensor_idx] = True
                    mins[sensor_idx] = value
                    maxs[sensor_idx] = value
                else:
                    if value < mins[sensor_idx]:
                        mins[sensor_idx] = value
                    if value > maxs[sensor_idx]:
                        maxs[sensor_idx] = value

        time.sleep(0.0005)
    return mins, maxs, seen


def fetch_trace(
    bus,
    board_id: int,
    sensor_idx: int,
    *,
    chunk_count: int,
    chunk_samples: int,
    timeout_s: float = 0.05,
):
    samples = [0] * (chunk_count * chunk_samples)
    valid_samples = 0
    for chunk_idx in range(chunk_count):
        payload = bytes([sensor_idx & 0xFF, chunk_idx & 0xFF])
        bus.send_frame(FRAME_TYPE_TRACE_REQ, board_id, payload, chunk_idx & 0xFFFF)
        deadline = time.monotonic() + timeout_s
        received = False
        while time.monotonic() < deadline:
            for frame_type, target_id, resp, seq in bus.read_frames():
                if frame_type != FRAME_TYPE_TRACE_RESP or target_id != board_id:
                    continue
                if len(resp) < 4:
                    continue
                if resp[0] != sensor_idx or resp[1] != chunk_idx:
                    continue
                valid_samples = int.from_bytes(resp[2:4], "little")
                data = resp[4:]
                limit = min(chunk_samples, len(data) // 2)
                base = chunk_idx * chunk_samples
                for i in range(limit):
                    offset = i * 2
                    samples[base + i] = int.from_bytes(data[offset : offset + 2], "little")
                received = True
                break
            if received:
                break
            time.sleep(0.0005)
    if valid_samples and valid_samples < len(samples):
        samples = samples[:valid_samples]
    return samples, valid_samples
