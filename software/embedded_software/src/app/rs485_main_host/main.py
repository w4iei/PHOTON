"""RS-485 main host (run mode 'rs485-main-host')."""

from __future__ import annotations

import json

from app.helpers.midi_play import midi_setup
from app.helpers.utils import log_note
from app.rs485_common.config import deep_merge, load_config

from .config import parse_disabled_sensors
from .constants import (
    CONFIG_PATH,
    DEFAULT_CONFIG,
    MIDI_HIGH_F,
    MIDI_LOW_FF,
    MIN_SENSOR_RANGE,
    SENSOR_NODE_DEVICE_IDS,
    SENSORS_PER_MANUAL,
    TOTAL_SENSORS,
    UART_BAUD,
    ENABLE_DISPLAY,
    ENABLE_TOUCH,
)
from .console import print_disabled_sensor_warning, print_usb_commands_box
from .display import disable_display, try_init_display, try_init_touch, update_bars
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


def main() -> None:
    setup_output(RS485_TERM_PIN, value=True)
    bus, use_fast = setup_rs485()

    cfg = deep_merge(DEFAULT_CONFIG, load_config(CONFIG_PATH))
    event_mode = bool(cfg.get("event_mode", False))
    send_midi = bool(cfg.get("send_midi", True))
    log_midi_events = bool(cfg.get("log_midi_events", False))
    min_range = int(cfg.get("min_range", MIN_SENSOR_RANGE))
    if min_range < 0:
        min_range = 0
    disabled_sensors = parse_disabled_sensors(cfg.get("disabled_sensors"))
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

    print("")
    print(".*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*")
    print(".*        PHOTON RS485 Main Host        .*")
    print(".*.   https://github.com/w4iei/photon   .*")
    print(".*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*")
    print("Creator: Noah Jaffe")
    print("")
    print("--- RS485 Main Host ---")
    print(f"UART @ {UART_BAUD} baud")
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
    rs485_driver_name = "photon_rs485" if use_fast else "python"
    rs485_driver_kind = "C driver" if use_fast else "Python driver"
    print(f"RS485 driver: {rs485_driver_name} ({rs485_driver_kind})")
    print(f"Event mode: {event_mode}")
    print_disabled_sensor_warning(disabled_count)
    print("Config:")
    try:
        print(json.dumps(cfg, sort_keys=True, indent=2))
    except TypeError:
        try:
            print(json.dumps(cfg))
        except TypeError:
            print(cfg)

    display = None
    bar_bitmaps = None
    bar_w = 0
    bar_h = 0
    if ENABLE_DISPLAY:
        display, bar_bitmaps, bar_w, bar_h = try_init_display()
    else:
        disable_display()
    touch_dev = try_init_touch() if ENABLE_TOUCH else None
    if display is None:
        print("Display disabled; continuing headless.")
    _ = touch_dev

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

    display_enabled = ENABLE_DISPLAY and bar_bitmaps is not None
    update_display = None
    if display_enabled:
        def update_display(values):
            update_bars(bar_bitmaps, values, bar_w, bar_h)

    run_polling_mode(
        bus,
        min_range=min_range,
        index_to_midi=index_to_midi,
        logical_index=logical_index,
        display_enabled=display_enabled,
        update_display=update_display,
    )


if __name__ == "__main__":
    main()
