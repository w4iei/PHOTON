"""Event mode handling for the RS-485 main host."""

from __future__ import annotations

import time

from app.midi_play import midi_note_off, midi_note_on
from app.utils import log_note, midi_to_name, read_serial_input, safe_print, serial_poll_requested, toggle_usb_logging
from photon_rs485 import (
    FRAME_TYPE_EVENT,
    FRAME_TYPE_EVENT_ACK,
    FRAME_TYPE_MINMAX_RESP,
    FRAME_TYPE_PONG,
    FRAME_TYPE_TRACE_RESP,
)

from .calibration import handle_cal_reset, handle_cal_save
from .constants import (
    DATA_RESPONSE_TIMEOUT_S,
    DATA_TIMEOUT_S,
    HEALTH_CHECK_IDLE_S,
    MAX_SENSORS,
    PRINT_INTERVAL_S,
    SENSOR_NODE_DEVICE_IDS,
    SENSOR_VALUE_MAX,
    TOTAL_SENSORS,
    TRACE_CHUNK_SAMPLES,
)
from .console import print_full_table
from .midi_mapping import global_index
from .protocol import fetch_trace, health_check, ping_all_sequential, poll_all_sensor_nodes, print_health_results


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
) -> int:
    if dt_ms <= 0 or dt_ms <= min_ms:
        return 127
    if dt_ms >= max_ms:
        return 1
    scale = (max_ms - dt_ms) / max(max_ms - min_ms, 0.001)
    if curve <= 0:
        curve = 1.0
    if curve != 1.0:
        try:
            scale = pow(scale, curve)
        except Exception:
            pass
    return int(1 + (scale * 126))


def _log_midi_event(
    label: str,
    note: int,
    channel: int,
    sensor_idx: int,
    velocity: int | None,
    dt_ms: int | None,
    velocity_curve: float | None,
) -> None:
    vel_label = "??" if velocity is None else str(int(velocity))
    dt_label = "??" if dt_ms is None else str(int(dt_ms))
    curve_label = "??" if velocity_curve is None else f"{velocity_curve:.3f}"
    board_slot = sensor_idx // MAX_SENSORS
    board_id = SENSOR_NODE_DEVICE_IDS[board_slot] if board_slot < len(SENSOR_NODE_DEVICE_IDS) else -1
    local_idx = sensor_idx % MAX_SENSORS
    log_note(
        "MIDI %s ch=%d note=%d(%s) vel=%s dt_ms=%s curve=%s board=%d sensor=%d"
        % (label, channel + 1, note, midi_to_name(note), vel_label, dt_label, curve_label, board_id, local_idx)
    )


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
            _log_midi_event("ON", note, channel, sensor_idx, velocity, dt_ms, velocity_curve)
    elif not is_active and was_active:
        midi_note_off(note, channel=channel, velocity=velocity)
        note_active[key] = False
        if log_midi_events:
            _log_midi_event("OFF", note, channel, sensor_idx, velocity, dt_ms, velocity_curve)


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
        safe_print(f"EVENT node={target_id} payload_len={len(payload)} (too short)")
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
    safe_print(
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

    def _handle_event_frame(frame_type, target_id, source_id, payload, seq, now) -> None:
        nonlocal event_on, event_off, last_event_rx, next_health_check
        if frame_type == FRAME_TYPE_EVENT:
            if len(payload) < 2:
                return
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
            bus.send_frame(FRAME_TYPE_EVENT_ACK, source_id, b"", event_seq, ack_timeout_us=0)
            if last_event_seq.get(source_id) == event_seq:
                return
            last_event_seq[source_id] = event_seq
            event_counts[source_id] = event_counts.get(source_id, 0) + 1
            if state:
                event_on += 1
            else:
                event_off += 1
            if dt_ms is not None:
                velocity = velocity_from_dt_ms(dt_ms, velocity_min_ms, velocity_max_ms, velocity_curve)
                if log_velocity_details:
                    scale = (velocity_max_ms - dt_ms) / max(velocity_max_ms - velocity_min_ms, 0.001)
                    safe_print(
                        "[RS485 main] VELOCITY node=%d sensor=%d dt_ms=%.2f vel=%d scale=%.4f"
                        % (source_id, sensor_idx, dt_ms, velocity, scale)
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
        elif frame_type in (FRAME_TYPE_PONG, FRAME_TYPE_MINMAX_RESP, FRAME_TYPE_TRACE_RESP):
            last_rx[source_id] = now

    def _do_health_check() -> None:
        nonlocal stats_seq, health_seen, health_warned
        stats_responses, stats_seq = health_check(bus, stats_seq, on_frame=_handle_event_frame)
        health_seen, health_warned = print_health_results(
            stats_responses, health_seen=health_seen, health_warned=health_warned
        )

    safe_print("Event mode active; waiting for sensor events.")
    cal_seq_ref = [0]
    _do_health_check()
    next_health_check = time.monotonic() + HEALTH_CHECK_IDLE_S
    while True:
        now = time.monotonic()
        for frame_type, target_id, source_id, payload, seq in bus.read_frames():
            _handle_event_frame(frame_type, target_id, source_id, payload, seq, now)

        stale_nodes = [
            nid for nid in SENSOR_NODE_DEVICE_IDS
            if last_rx.get(nid) is None or (now - last_rx[nid]) > DATA_TIMEOUT_S
        ]
        if stale_nodes:
            ping_seq, _ = ping_all_sequential(
                bus, stale_nodes, ping_seq, on_frame=_handle_event_frame,
            )

        if now >= next_health_check:
            if last_event_rx is None or (now - last_event_rx) >= HEALTH_CHECK_IDLE_S:
                _do_health_check()
                next_health_check = now + HEALTH_CHECK_IDLE_S

        if now >= next_print:
            for sensor_device_id in SENSOR_NODE_DEVICE_IDS:
                count = event_counts.get(sensor_device_id, 0)
                safe_print(f"# events sensor node {sensor_device_id}: {count}")
                event_counts[sensor_device_id] = 0
            safe_print(f"# events total: on={event_on} off={event_off}")
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
                elif ch in ("p", "P"):
                    toggle_usb_logging()

        time.sleep(0.0005)
