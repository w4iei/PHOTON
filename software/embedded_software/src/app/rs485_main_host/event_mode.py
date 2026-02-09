"""Event mode handling for the RS-485 main host."""

from __future__ import annotations

import time

from app.helpers.midi_play import midi_note_off, midi_note_on
from app.helpers.utils import log_note, midi_to_name
from app.rs485_bus import (
    FRAME_TYPE_EVENT,
    FRAME_TYPE_EVENT_ACK,
    FRAME_TYPE_MINMAX_RESP,
    FRAME_TYPE_PING,
    FRAME_TYPE_PONG,
    FRAME_TYPE_STATS_REQ,
    FRAME_TYPE_STATS_RESP,
    FRAME_TYPE_TRACE_RESP,
)
from app.rs485_common.serial import read_serial_input, serial_poll_requested

from .calibration import handle_cal_reset, handle_cal_save
from .constants import (
    DATA_RESPONSE_TIMEOUT_S,
    DATA_TIMEOUT_S,
    HEALTH_CHECK_IDLE_S,
    MAX_SENSORS,
    PRINT_INTERVAL_S,
    SENSOR_NODE_DEVICE_IDS,
    SENSOR_VALUE_MAX,
    STATS_RESPONSE_TIMEOUT_S,
    TOTAL_SENSORS,
    TRACE_CHUNK_SAMPLES,
)
from .console import print_full_table
from .midi_mapping import global_index
from .protocol import fetch_trace, poll_all_sensor_nodes, send_with_gap


def clamp_curve(value, default):
    try:
        value = float(value)
    except Exception:
        return default
    if value <= 0:
        return default
    if value > 10.0:
        return 10.0
    return value


def velocity_from_dt_ms(
    dt_ms: float,
    min_ms: float,
    max_ms: float,
    curve: float,
    debug: dict | None = None,
) -> int:
    if dt_ms <= 0:
        if debug is not None:
            debug.update(
                {
                    "dt_ms": dt_ms,
                    "min_ms": min_ms,
                    "max_ms": max_ms,
                    "curve": curve,
                    "reason": "dt<=0",
                    "velocity": 127,
                }
            )
        return 127
    if dt_ms <= min_ms:
        if debug is not None:
            debug.update(
                {
                    "dt_ms": dt_ms,
                    "min_ms": min_ms,
                    "max_ms": max_ms,
                    "curve": curve,
                    "reason": "dt<=min_ms",
                    "velocity": 127,
                }
            )
        return 127
    if dt_ms >= max_ms:
        if debug is not None:
            debug.update(
                {
                    "dt_ms": dt_ms,
                    "min_ms": min_ms,
                    "max_ms": max_ms,
                    "curve": curve,
                    "reason": "dt>=max_ms",
                    "velocity": 1,
                }
            )
        return 1
    scale = (max_ms - dt_ms) / max(max_ms - min_ms, 0.001)
    scale_raw = scale
    if curve <= 0:
        curve = 1.0
    if curve != 1.0:
        try:
            scale = pow(scale, curve)
        except Exception:
            pass
    velocity = int(1 + (scale * 126))
    if debug is not None:
        debug.update(
            {
                "dt_ms": dt_ms,
                "min_ms": min_ms,
                "max_ms": max_ms,
                "curve": curve,
                "scale_raw": scale_raw,
                "scale": scale,
                "reason": "scaled",
                "velocity": velocity,
            }
        )
    return velocity


def event_source_id(target_id: int, payload: bytes) -> int:
    if len(payload) >= 7:
        source_id = payload[6]
        if source_id in SENSOR_NODE_DEVICE_IDS:
            return source_id
    if len(payload) >= 6:
        source_id = payload[5]
        if source_id in SENSOR_NODE_DEVICE_IDS:
            return source_id
    return target_id


def update_note_state(
    sensor_idx: int,
    state: int,
    *,
    dt_ms: int | None = None,
    velocity: int | None = None,
    velocity_curve: float | None = None,
    index_to_midi,
    index_to_channel,
    note_sensors,
    note_active,
    sensor_active,
    send_midi: bool,
    log_midi_events: bool,
) -> None:
    if not send_midi:
        return
    if sensor_idx is None or sensor_idx < 0 or sensor_idx >= len(sensor_active):
        return
    note = index_to_midi.get(sensor_idx)
    if note is None:
        return
    channel = index_to_channel.get(sensor_idx)
    if channel is None:
        return
    sensor_active[sensor_idx] = bool(state)
    key = (channel, note)
    sensors = note_sensors.get(key, [])
    is_active = any(sensor_active[idx] for idx in sensors)
    was_active = note_active.get(key, False)
    if is_active and not was_active:
        midi_note_on(note, channel=channel, velocity=velocity)
        note_active[key] = True
        if log_midi_events:
            vel_label = "??" if velocity is None else str(int(velocity))
            dt_label = "??" if dt_ms is None else str(int(dt_ms))
            curve_label = "??" if velocity_curve is None else f"{velocity_curve:.3f}"
            board_slot = sensor_idx // MAX_SENSORS
            board_id = SENSOR_NODE_DEVICE_IDS[board_slot] if board_slot < len(SENSOR_NODE_DEVICE_IDS) else -1
            local_idx = sensor_idx % MAX_SENSORS
            log_note(
                "MIDI ON ch=%d note=%d(%s) vel=%s dt_ms=%s curve=%s board=%d sensor=%d"
                % (
                    channel + 1,
                    note,
                    midi_to_name(note),
                    vel_label,
                    dt_label,
                    curve_label,
                    board_id,
                    local_idx,
                )
            )
    elif not is_active and was_active:
        midi_note_off(note, channel=channel, velocity=velocity)
        note_active[key] = False
        if log_midi_events:
            vel_label = "??" if velocity is None else str(int(velocity))
            dt_label = "??" if dt_ms is None else str(int(dt_ms))
            curve_label = "??" if velocity_curve is None else f"{velocity_curve:.3f}"
            board_slot = sensor_idx // MAX_SENSORS
            board_id = SENSOR_NODE_DEVICE_IDS[board_slot] if board_slot < len(SENSOR_NODE_DEVICE_IDS) else -1
            local_idx = sensor_idx % MAX_SENSORS
            log_note(
                "MIDI OFF ch=%d note=%d(%s) vel=%s dt_ms=%s curve=%s board=%d sensor=%d"
                % (
                    channel + 1,
                    note,
                    midi_to_name(note),
                    vel_label,
                    dt_label,
                    curve_label,
                    board_id,
                    local_idx,
                )
            )


def log_event(
    target_id: int,
    payload: bytes,
    seq: int,
    *,
    dt_ms: int | None = None,
    velocity: int | None = None,
    index_to_midi=None,
    index_to_channel=None,
) -> None:
    if len(payload) < 2:
        print(f"EVENT node={target_id} payload_len={len(payload)} (too short)")
        return
    sensor_idx = payload[0]
    state = payload[1]
    event_seq = seq
    if len(payload) >= 7:
        if dt_ms is None:
            dt_ms = int.from_bytes(payload[2:4], "little")
        event_seq = int.from_bytes(payload[4:6], "little")
    elif len(payload) >= 5:
        if velocity is None and len(payload) >= 3:
            velocity = payload[2]
        event_seq = int.from_bytes(payload[3:5], "little")
    elif len(payload) >= 3 and velocity is None:
        velocity = payload[2]
    global_idx = global_index(target_id, sensor_idx)
    midi_note = None
    note_label = "None"
    channel = None
    if index_to_midi is not None and global_idx is not None:
        midi_note = index_to_midi.get(global_idx)
        note_label = midi_to_name(midi_note)
    if index_to_channel is not None and global_idx is not None:
        channel = index_to_channel.get(global_idx)
    state_label = "ON" if state else "OFF"
    dt_label = dt_ms if dt_ms is not None else "??"
    vel_label = velocity if velocity is not None else "??"
    print(
        f"EVENT node={target_id} sensor={sensor_idx} global={global_idx} "
        f"midi={midi_note} note={note_label} ch={channel + 1 if channel is not None else '??'} "
        f"state={state_label} dt_ms={dt_label} vel={vel_label} seq={event_seq}"
    )


def run_event_mode(
    bus,
    cfg: dict,
    *,
    index_to_midi,
    index_to_channel,
    logical_index,
    note_sensors,
    note_active,
    sensor_active,
    send_midi: bool,
    log_midi_events: bool,
    min_range: int,
) -> None:
    velocity_min_ms = float(cfg.get("velocity_min_ms", 8.0))
    velocity_max_ms = float(cfg.get("velocity_max_ms", 100.0))
    velocity_curve = clamp_curve(cfg.get("velocity_curve", 1.0), 1.0)
    log_velocity_details = bool(cfg.get("log_velocity_details", False))
    trace_sample_hz = int(cfg.get("trace_sample_hz", 100))
    trace_seconds = int(cfg.get("trace_seconds", 10))
    trace_fetch_on_off = bool(cfg.get("trace_fetch_on_off", False))
    log_events = bool(cfg.get("log_events", False))
    manual_values = [0] * TOTAL_SENSORS
    manual_values_seen = [False] * TOTAL_SENSORS
    manual_stds = [0] * TOTAL_SENSORS
    manual_std_seen = [False] * TOTAL_SENSORS
    manual_mins = [SENSOR_VALUE_MAX] * TOTAL_SENSORS
    manual_maxs = [0] * TOTAL_SENSORS
    manual_minmax_seen = [False] * TOTAL_SENSORS
    bus_max_payload = getattr(bus, "max_payload", MAX_SENSORS * 6)
    trace_samples = max(0, trace_sample_hz * trace_seconds)
    trace_chunk_samples = min(TRACE_CHUNK_SAMPLES, max((bus_max_payload - 4) // 2, 0))
    trace_chunk_count = (
        (trace_samples + trace_chunk_samples - 1) // trace_chunk_samples if trace_chunk_samples else 0
    )

    last_rx = {sensor_device_id: time.monotonic() for sensor_device_id in SENSOR_NODE_DEVICE_IDS}
    last_event_seq = {sensor_device_id: None for sensor_device_id in SENSOR_NODE_DEVICE_IDS}
    event_counts = {sensor_device_id: 0 for sensor_device_id in SENSOR_NODE_DEVICE_IDS}
    event_on = 0
    event_off = 0
    next_print = time.monotonic() + PRINT_INTERVAL_S
    ping_seq = 0
    stats_seq = 0
    last_event_rx = None
    next_health_check = time.monotonic() + HEALTH_CHECK_IDLE_S
    health_seen = False
    health_warned = False

    def _handle_event_frame(frame_type, target_id, payload, seq, now) -> None:
        nonlocal event_on, event_off, last_event_rx, next_health_check
        if frame_type == FRAME_TYPE_EVENT:
            if len(payload) < 2:
                return
            source_id = event_source_id(target_id, payload)
            last_rx[source_id] = now
            last_event_rx = now
            next_health_check = now + HEALTH_CHECK_IDLE_S
            sensor_idx = payload[0]
            state = payload[1]
            dt_ms = None
            velocity = None
            event_seq = seq
            if len(payload) >= 7:
                dt_ms = int.from_bytes(payload[2:4], "little")
                event_seq = int.from_bytes(payload[4:6], "little")
            elif len(payload) >= 5:
                velocity = payload[2]
                event_seq = int.from_bytes(payload[3:5], "little")
            elif len(payload) >= 3:
                velocity = payload[2]
            bus.send_frame(FRAME_TYPE_EVENT_ACK, source_id, b"", event_seq)
            if last_event_seq.get(source_id) == event_seq:
                return
            last_event_seq[source_id] = event_seq
            event_counts[source_id] = event_counts.get(source_id, 0) + 1
            if state:
                event_on += 1
            else:
                event_off += 1
            if dt_ms is not None:
                velocity_debug = {} if log_velocity_details else None
                velocity = velocity_from_dt_ms(
                    dt_ms,
                    velocity_min_ms,
                    velocity_max_ms,
                    velocity_curve,
                    debug=velocity_debug,
                )
                if log_velocity_details and velocity_debug is not None:
                    print(
                        "[RS485 main] VELOCITY node=%d sensor=%d dt_ms=%.2f min_ms=%.2f "
                        "max_ms=%.2f curve=%.3f scale=%.4f raw=%.4f reason=%s vel=%d"
                        % (
                            source_id,
                            sensor_idx,
                            velocity_debug.get("dt_ms", 0.0),
                            velocity_debug.get("min_ms", 0.0),
                            velocity_debug.get("max_ms", 0.0),
                            velocity_debug.get("curve", 1.0),
                            velocity_debug.get("scale", 0.0),
                            velocity_debug.get("scale_raw", 0.0),
                            velocity_debug.get("reason", "unknown"),
                            velocity_debug.get("velocity", velocity),
                        )
                    )
            global_idx = global_index(source_id, sensor_idx)
            if global_idx is not None:
                update_note_state(
                    global_idx,
                    state,
                    dt_ms=dt_ms,
                    velocity=velocity,
                    velocity_curve=velocity_curve,
                    index_to_midi=index_to_midi,
                    index_to_channel=index_to_channel,
                    note_sensors=note_sensors,
                    note_active=note_active,
                    sensor_active=sensor_active,
                    send_midi=send_midi,
                    log_midi_events=log_midi_events,
                )
            if log_events:
                log_event(
                    source_id,
                    payload,
                    seq,
                    dt_ms=dt_ms,
                    velocity=velocity,
                    index_to_midi=index_to_midi,
                    index_to_channel=index_to_channel,
                )
            if trace_fetch_on_off and (not state) and trace_chunk_count > 0:
                fetch_trace(
                    bus,
                    source_id,
                    sensor_idx,
                    chunk_count=trace_chunk_count,
                    chunk_samples=trace_chunk_samples,
                )
        elif frame_type == FRAME_TYPE_PONG:
            last_rx[target_id] = now
        elif frame_type == FRAME_TYPE_MINMAX_RESP:
            last_rx[target_id] = now
        elif frame_type == FRAME_TYPE_TRACE_RESP:
            last_rx[target_id] = now

    def _collect_stats(seq: int, timeout_s: float):
        responses = {}
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            for frame_type, rx_target_id, payload, rx_seq in bus.read_frames():
                if frame_type == FRAME_TYPE_STATS_RESP and rx_seq == seq:
                    last_rx[rx_target_id] = time.monotonic()
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
                _handle_event_frame(frame_type, rx_target_id, payload, rx_seq, time.monotonic())
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

    print("Event mode active; waiting for sensor events.")
    cal_seq_ref = [0]
    _health_check()
    next_health_check = time.monotonic() + HEALTH_CHECK_IDLE_S
    while True:
        now = time.monotonic()
        for frame_type, target_id, payload, seq in bus.read_frames():
            _handle_event_frame(frame_type, target_id, payload, seq, now)

        for sensor_device_id in SENSOR_NODE_DEVICE_IDS:
            last_rx_ts = last_rx.get(sensor_device_id)
            if last_rx_ts is None or (now - last_rx_ts) > DATA_TIMEOUT_S:
                send_with_gap(bus, FRAME_TYPE_PING, sensor_device_id, b"", ping_seq & 0xFFFF)
                ping_seq += 1

        if now >= next_health_check:
            if last_event_rx is None or (now - last_event_rx) >= HEALTH_CHECK_IDLE_S:
                _health_check()
                next_health_check = now + HEALTH_CHECK_IDLE_S

        if now >= next_print:
            for sensor_device_id in SENSOR_NODE_DEVICE_IDS:
                count = event_counts.get(sensor_device_id, 0)
                print(f"# events sensor node {sensor_device_id}: {count}")
                event_counts[sensor_device_id] = 0
            print(f"# events total: on={event_on} off={event_off}")
            event_on = 0
            event_off = 0
            next_print = now + PRINT_INTERVAL_S

        if serial_poll_requested():
            data = read_serial_input()
            for ch in data:
                if ch in ("\n", "\r"):
                    poll_all_sensor_nodes(
                        bus,
                        manual_values,
                        manual_values_seen,
                        manual_stds,
                        manual_std_seen,
                        manual_mins,
                        manual_maxs,
                        manual_minmax_seen,
                        timeout_s=DATA_RESPONSE_TIMEOUT_S,
                        on_event=_handle_event_frame,
                    )
                    print_full_table(
                        manual_values,
                        manual_values_seen,
                        manual_stds,
                        manual_std_seen,
                        manual_mins,
                        manual_maxs,
                        manual_minmax_seen,
                        index_to_midi,
                        logical_index,
                        min_range,
                    )
                elif ch in ("s", "S"):
                    handle_cal_save(bus, cal_seq_ref)
                elif ch in ("r", "R"):
                    handle_cal_reset(bus, cal_seq_ref)

        time.sleep(0.0005)
