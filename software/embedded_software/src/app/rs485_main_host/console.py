"""Console output helpers for the RS-485 main host."""

from __future__ import annotations

from app.helpers.utils import midi_to_name

from .constants import MAX_SENSORS, SENSOR_NODE_DEVICE_IDS, TOTAL_SENSORS


def print_usb_commands_box() -> None:
    command_lines = [
        "Press Enter to print the sensor table over USB serial.",
        "[S] Save calibration + re-enable MSC on next boot.",
        "[R] Reboot into calibration mode (disable MSC).",
    ]
    frame_width = max(len(line) for line in command_lines)
    print("+-" + ("-" * frame_width) + "-+")
    for line in command_lines:
        padding = frame_width - len(line)
        if padding < 0:
            padding = 0
        print("| " + line + (" " * padding) + " |")
    print("+-" + ("-" * frame_width) + "-+")


def print_disabled_sensor_warning(disabled_count: int) -> None:
    if disabled_count <= 0:
        return
    lines = [
        f"WARNING: {disabled_count} sensor(s) disabled.",
        "Disabled due to high level at boot OR config override.",
        "Please be sure that no keys are pressed",
        "when powering on the photon system.",
        "Config source: /config/rs485_main_host.json -> disabled_sensors",
    ]
    frame_width = max(len(line) for line in lines)
    print("+-" + ("-" * frame_width) + "-+")
    for line in lines:
        padding = frame_width - len(line)
        if padding < 0:
            padding = 0
        print("| " + line + (" " * padding) + " |")
    print("+-" + ("-" * frame_width) + "-+")


def _format_cell(val) -> str:
    return str(val)


def print_full_table(
    values,
    values_seen,
    stds,
    std_seen,
    mins,
    maxs,
    minmax_seen,
    index_to_midi,
    logical_index,
    min_range: int,
) -> None:
    header = [
        "lidx",
        "gidx",
        "brd",
        "sen",
        "midi",
        "note",
        "val",
        "min",
        "max",
        "rng",
        "std",
        "ena",
    ]
    print("\n# Sensor snapshot (manual poll)")
    print(
        "{:>4s} | {:>4s} | {:>3s} | {:>3s} | {:>4s} | {:>4s} | {:>5s} | {:>5s} | {:>5s} | {:>5s} | {:>5s} | {:>3s}".format(
            *header
        )
    )
    print("-" * 91)
    for idx in range(TOTAL_SENSORS):
        lidx = logical_index.get(idx)
        lidx_str = "--" if lidx is None else str(lidx)
        board_slot = idx // MAX_SENSORS
        board_id = SENSOR_NODE_DEVICE_IDS[board_slot] if board_slot < len(SENSOR_NODE_DEVICE_IDS) else -1
        sensor_id = idx % MAX_SENSORS
        note = index_to_midi.get(idx)
        midi_str = "None" if note is None else str(int(note))
        note_str = midi_to_name(note)
        ena = "0"
        if values_seen[idx]:
            val_s = _format_cell(values[idx])
        else:
            val_s = "None"
        if minmax_seen[idx]:
            min_v = mins[idx]
            max_v = maxs[idx]
            rng = max_v - min_v
            min_s = _format_cell(min_v)
            max_s = _format_cell(max_v)
            rng_s = _format_cell(rng)
            if note is not None and rng >= min_range:
                ena = "1"
        else:
            min_s = "None"
            max_s = "None"
            rng_s = "None"
        if std_seen[idx]:
            std_s = _format_cell(stds[idx])
        else:
            std_s = "None"
        print(
            "{:>4s} | {:>4d} | {:>3d} | {:>3d} | {:>4s} | {:>4s} | {:>5s} | {:>5s} | {:>5s} | {:>5s} | {:>5s} | {:>3s}".format(
                lidx_str,
                idx,
                board_id,
                sensor_id,
                midi_str,
                note_str,
                val_s,
                min_s,
                max_s,
                rng_s,
                std_s,
                ena,
            )
        )


def print_table(
    board_id: int,
    values,
    values_seen,
    stds,
    std_seen,
    mins,
    maxs,
    minmax_seen,
    index_to_midi,
    min_range: int,
) -> None:
    header = ["idx", "midi", "note", "val", "min", "max", "rng", "std", "ena"]
    print(f"\n# Board {board_id}")
    print(
        "{:>3s} | {:>4s} | {:>4s} | {:>5s} | {:>5s} | {:>5s} | {:>5s} | {:>5s} | {:>3s}".format(
            *header
        )
    )
    print("-" * 77)
    for sensor_id in range(32):
        idx = ((board_id - 1) * 32) + sensor_id
        if idx < 0 or idx >= len(values):
            continue
        note = index_to_midi.get(idx)
        midi_str = "None" if note is None else str(int(note))
        note_str = midi_to_name(note)
        ena = "0"
        if values_seen[idx]:
            val_s = _format_cell(values[idx])
        else:
            val_s = "None"
        if minmax_seen[idx]:
            min_v = mins[idx]
            max_v = maxs[idx]
            rng = max_v - min_v
            min_s = _format_cell(min_v)
            max_s = _format_cell(max_v)
            rng_s = _format_cell(rng)
            if note is not None and rng >= min_range:
                ena = "1"
        else:
            min_s = "None"
            max_s = "None"
            rng_s = "None"
        if std_seen[idx]:
            std_s = _format_cell(stds[idx])
        else:
            std_s = "None"
        row = [
            f"{sensor_id}",
            midi_str,
            note_str,
            val_s,
            min_s,
            max_s,
            rng_s,
            std_s,
            ena,
        ]
        print(
            "{:>3s} | {:>4s} | {:>4s} | {:>5s} | {:>5s} | {:>5s} | {:>5s} | {:>5s} | {:>3s}".format(
                *row
            )
        )
