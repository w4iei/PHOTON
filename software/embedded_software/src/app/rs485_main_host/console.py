"""Console output helpers for the RS-485 main host."""

from __future__ import annotations

from app.utils import midi_to_name, print_box, print_disabled_sensor_warning

from .constants import MAX_SENSORS, SENSOR_NODE_DEVICE_IDS, TOTAL_SENSORS


def print_usb_commands_box() -> None:
    print_box([
        "Press Enter to print the sensor table over USB serial.",
        "[S] Save calibration + re-enable MSC on next boot.",
        "[R] Reboot into calibration mode (disable MSC).",
        "[P] Toggle USB serial logging on/off.",
    ])


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
            val_s = str(values[idx])
        else:
            val_s = "None"
        if minmax_seen[idx]:
            min_v = mins[idx]
            max_v = maxs[idx]
            rng = max_v - min_v
            min_s = str(min_v)
            max_s = str(max_v)
            rng_s = str(rng)
            if note is not None and rng >= min_range:
                ena = "1"
        else:
            min_s = "None"
            max_s = "None"
            rng_s = "None"
        if std_seen[idx]:
            std_s = str(stds[idx])
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
            val_s = str(values[idx])
        else:
            val_s = "None"
        if minmax_seen[idx]:
            min_v = mins[idx]
            max_v = maxs[idx]
            rng = max_v - min_v
            min_s = str(min_v)
            max_s = str(max_v)
            rng_s = str(rng)
            if note is not None and rng >= min_range:
                ena = "1"
        else:
            min_s = "None"
            max_s = "None"
            rng_s = "None"
        if std_seen[idx]:
            std_s = str(stds[idx])
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
