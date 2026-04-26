"""RS-485 protocol helpers for the main host."""

from __future__ import annotations

import time

from photon_rs485 import (
    FRAME_TYPE_DATA_REQ,
    FRAME_TYPE_DATA_RESP,
    FRAME_TYPE_EVENT,
    FRAME_TYPE_MINMAX_REQ,
    FRAME_TYPE_MINMAX_RESP,
    FRAME_TYPE_PING,
    FRAME_TYPE_PONG,
    FRAME_TYPE_STATS_REQ,
    FRAME_TYPE_STATS_RESP,
    FRAME_TYPE_TRACE_REQ,
    FRAME_TYPE_TRACE_RESP,
)

from app.utils import safe_print

from .constants import (
    DATA_RECORD_BYTES,
    MAX_SENSORS,
    MINMAX_RECORD_BYTES,
    PING_RESPONSE_TIMEOUT_S,
    SENSOR_NODE_DEVICE_IDS,
    STATS_RESPONSE_TIMEOUT_S,
    TOTAL_SENSORS,
)


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


def ping_all_sequential(bus, node_ids, ping_seq: int, *, on_frame=None):
    """Ping each node one at a time, waiting up to PING_RESPONSE_TIMEOUT_S for a PONG."""
    pong_received = {}
    for nid in node_ids:
        seq = ping_seq & 0xFFFF
        ping_seq = (ping_seq + 1) & 0xFFFF
        bus.send_frame(FRAME_TYPE_PING, nid, b"", seq, ack_timeout_us=0)
        deadline = time.monotonic() + PING_RESPONSE_TIMEOUT_S
        while time.monotonic() < deadline:
            for frame_type, target_id, source_id, payload, rx_seq in bus.read_frames():
                if frame_type == FRAME_TYPE_PONG and source_id == nid:
                    pong_received[nid] = True
                elif on_frame is not None:
                    on_frame(frame_type, target_id, source_id, payload, rx_seq, time.monotonic())
            if nid in pong_received:
                break
            time.sleep(0.0005)
    return ping_seq, pong_received


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
        bus.send_frame(FRAME_TYPE_DATA_REQ, board_id, b"", seq & 0xFFFF, ack_timeout_us=0)
        seq = (seq + 1) & 0xFFFF
        deadline = time.monotonic() + timeout_s
        received = False
        while time.monotonic() < deadline:
            for frame_type, target_id, source_id, payload, rx_seq in bus.read_frames():
                if frame_type == FRAME_TYPE_DATA_RESP and source_id == board_id:
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
                    on_event(frame_type, target_id, source_id, payload, rx_seq, time.monotonic())
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
        bus.send_frame(FRAME_TYPE_MINMAX_REQ, board_id, payload, seq & 0xFFFF, ack_timeout_us=0)
        seq = (seq + 1) & 0xFFFF
        deadline = time.monotonic() + timeout_s
        received = False
        resp_count = 0
        while time.monotonic() < deadline:
            for frame_type, target_id, source_id, payload, rx_seq in bus.read_frames():
                if frame_type == FRAME_TYPE_MINMAX_RESP and source_id == board_id:
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
                    on_event(frame_type, target_id, source_id, payload, rx_seq, time.monotonic())
            if received:
                break
            time.sleep(0.0005)
        if not received:
            return
        if resp_count < count:
            return
        start += count


def collect_stats(bus, seq: int, timeout_s: float, expected_source_id: int, *, on_frame=None):
    """Request and collect STATS_RESP from a single node."""
    responses = {}
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        for frame_type, rx_target_id, rx_source_id, payload, rx_seq in bus.read_frames():
            if frame_type == FRAME_TYPE_STATS_RESP and rx_seq == seq:
                if rx_source_id != expected_source_id:
                    continue
                rate = None
                if len(payload) >= 2:
                    rate_tenths = int.from_bytes(payload[:2], "little")
                    rate = rate_tenths / 10.0
                evt_tx = evt_drop = evt_retry = None
                if len(payload) >= 8:
                    evt_tx = int.from_bytes(payload[2:4], "little")
                    evt_drop = int.from_bytes(payload[4:6], "little")
                    evt_retry = int.from_bytes(payload[6:8], "little")
                responses[rx_source_id] = (rate, evt_tx, evt_drop, evt_retry)
                return responses
            if on_frame is not None:
                on_frame(frame_type, rx_target_id, rx_source_id, payload, rx_seq, time.monotonic())
        time.sleep(0.0005)
    return responses


def health_check(bus, stats_seq: int, *, on_frame=None):
    """Poll all sensor nodes for stats, return (responses, new_stats_seq)."""
    stats_responses = {}
    for sensor_device_id in SENSOR_NODE_DEVICE_IDS:
        seq = stats_seq & 0xFFFF
        stats_seq = (stats_seq + 1) & 0xFFFF
        bus.send_frame(FRAME_TYPE_STATS_REQ, sensor_device_id, b"", seq, ack_timeout_us=0)
        stats_responses.update(
            collect_stats(bus, seq, STATS_RESPONSE_TIMEOUT_S, sensor_device_id, on_frame=on_frame)
        )
    return stats_responses, stats_seq


def print_health_results(stats_responses: dict, *, health_seen: bool, health_warned: bool):
    """Print health check results, return (health_seen, health_warned)."""
    if stats_responses:
        health_seen = True
    elif not health_seen and not health_warned:
        safe_print(
            "\n!!! RS485 HEALTH CHECK FAILED: no responses from any sensor node !!!\n"
            "    Check power, wiring, device_id, and baud rate.\n"
        )
        health_warned = True
    for sensor_device_id in SENSOR_NODE_DEVICE_IDS:
        stats = stats_responses.get(sensor_device_id)
        if stats is None:
            safe_print(f"# status sensor node {sensor_device_id}: no response")
            continue
        rate, evt_tx, evt_drop, evt_retry = stats
        rate_val = rate if rate is not None else 0.0
        if evt_tx is None:
            safe_print(f"# status sensor node {sensor_device_id}: alive rate={rate_val:.1f} Hz")
        else:
            safe_print(
                f"# status sensor node {sensor_device_id}: alive rate={rate_val:.1f} Hz "
                f"evt_tx={evt_tx} evt_drop={evt_drop} evt_retry={evt_retry}"
            )
    return health_seen, health_warned


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
        bus.send_frame(FRAME_TYPE_TRACE_REQ, board_id, payload, chunk_idx & 0xFFFF, ack_timeout_us=0)
        deadline = time.monotonic() + timeout_s
        received = False
        while time.monotonic() < deadline:
            for frame_type, target_id, source_id, resp, seq in bus.read_frames():
                if frame_type != FRAME_TYPE_TRACE_RESP or source_id != board_id:
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
