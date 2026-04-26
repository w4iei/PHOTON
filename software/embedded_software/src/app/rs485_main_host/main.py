"""RS-485 main host (run mode 'rs485-main-host')."""

from __future__ import annotations

import os

from app.midi_play import midi_setup
from app.rs485_system_config import parse_disabled_sensors, rs485_host_config
from app.sensor_calibration import extract_node_calibration, load_calibration_file
from app.utils import log_note, midi_to_name

from .constants import (
    CALIBRATION_PATH,
    MAX_SENSORS,
    MIDI_HIGH_F,
    MIDI_LOW_FF,
    MIN_SENSOR_RANGE,
    SENSOR_NODE_DEVICE_IDS,
    SENSORS_PER_MANUAL,
    TOTAL_SENSORS,
    UART_BAUD,
)
from .console import print_disabled_sensor_warning, print_usb_commands_box
from .event_mode import run_event_mode
from .hardware_setup import setup_output, setup_rs485
from .midi_mapping import (
    board_manual_index,
    build_index_to_channel,
    build_index_to_midi_by_manual,
    build_note_maps,
    print_midi_mapping,
)
from .pins import RS485_TERM_PIN
from .polling_mode import run_polling_mode


def _boot_cal_health_check(index_to_midi: dict) -> None:
    """Log sensors whose calibrated min is >30% of their range."""
    cal = load_calibration_file(CALIBRATION_PATH)
    if cal is None:
        log_note("CAL_HEALTH no calibration file found")
        return
    faults = []
    for board_id in SENSOR_NODE_DEVICE_IDS:
        node = extract_node_calibration(cal, board_id)
        if node is None:
            continue
        mins = node.get("min", [])
        maxs = node.get("max", [])
        board_idx = SENSOR_NODE_DEVICE_IDS.index(board_id)
        for sensor_idx in range(min(len(mins), len(maxs))):
            min_v = mins[sensor_idx]
            max_v = maxs[sensor_idx]
            rng = max_v - min_v
            if rng <= 0:
                continue
            if min_v > 0.3 * rng:
                global_idx = board_idx * MAX_SENSORS + sensor_idx
                note = index_to_midi.get(global_idx)
                faults.append(
                    "board=%d sensor=%d note=%d(%s) min=%d max=%d rng=%d"
                    % (board_id, sensor_idx, note or 0, midi_to_name(note), min_v, max_v, rng)
                )
    if faults:
        log_note("CAL_HEALTH %d sensor(s) with min >30%% of range:" % len(faults))
        for f in faults:
            log_note("CAL_HEALTH   %s" % f)
    else:
        log_note("CAL_HEALTH all sensors OK")


def main() -> None:
    setup_output(RS485_TERM_PIN, value=True)
    bus = setup_rs485()

    cfg = rs485_host_config()
    event_mode = bool(cfg.get("event_mode", False))
    send_midi = bool(cfg.get("send_midi", True))
    log_midi_events = bool(cfg.get("log_midi_events", False))
    min_range = int(cfg.get("min_range", MIN_SENSOR_RANGE))
    if min_range < 0:
        min_range = 0
    disabled_sensors = parse_disabled_sensors(cfg.get("disabled_sensors"), TOTAL_SENSORS)
    disabled_count = len(disabled_sensors)
    index_to_channel = build_index_to_channel()
    log_note(f"index_to_channel={index_to_channel}")
    index_to_midi = build_index_to_midi_by_manual(
        disabled_sensors,
        sensors_per_manual=SENSORS_PER_MANUAL,
        low_midi=MIDI_LOW_FF,
        high_midi=MIDI_HIGH_F,
    )
    active_indices = [idx for idx in range(TOTAL_SENSORS) if idx not in disabled_sensors]
    logical_index = {idx: lidx for lidx, idx in enumerate(active_indices)}
    note_sensors, note_active = build_note_maps(index_to_midi, index_to_channel)
    sensor_active = [False] * TOTAL_SENSORS
    if send_midi:
        midi_setup()
    print_midi_mapping(index_to_midi, logical_index, index_to_channel)
    _boot_cal_health_check(index_to_midi)

    print("")
    print(f"Board: {os.uname().machine}")
    print(f"CircuitPython build: {os.uname().version}")
    print("")
    print("")
    print(".*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*")
    print(".*        PHOTON RS485 Main Host        .*")
    print(".*.   https://github.com/w4iei/photon   .*")
    print(".*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*")
    print("Creator: Noah Jaffe")
    print("")
    print("--- RS485 Main Host ---")
    print(f"Sensor nodes: {SENSOR_NODE_DEVICE_IDS}")
    channel_map = {}
    for board_id in SENSOR_NODE_DEVICE_IDS:
        channel = board_manual_index(board_id)
        if channel is None:
            continue
        channel_map.setdefault(channel, []).append(board_id)
    if channel_map:
        channel_entries = [
            f"ch{channel + 1}: {channel_map[channel]}" for channel in sorted(channel_map)
        ]
        print(f"MIDI channels: {', '.join(channel_entries)}")
    print(f"RS485 UART baud rate: {UART_BAUD / 1_000_000.0:.2f} MHz")
    print("RS485 driver: photon_rs485 (C driver)")
    print(f"Event mode: {event_mode}")
    print_disabled_sensor_warning(disabled_count)
    print("Config:")
    print(cfg)

    print_usb_commands_box()

    if event_mode:
        run_event_mode(
            bus,
            cfg,
            index_to_midi=index_to_midi,
            index_to_channel=index_to_channel,
            logical_index=logical_index,
            note_sensors=note_sensors,
            note_active=note_active,
            sensor_active=sensor_active,
            send_midi=send_midi,
            log_midi_events=log_midi_events,
            min_range=min_range,
        )
        return

    run_polling_mode(
        bus,
        min_range=min_range,
        index_to_midi=index_to_midi,
        logical_index=logical_index,
    )


if __name__ == "__main__":
    main()
