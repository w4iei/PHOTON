"""MIDI mapping helpers for the RS-485 main host."""

from __future__ import annotations

from app.midi_play import build_index_to_midi_linear
from app.utils import midi_to_name

from .constants import (
    BOARD_PAIR_SIZE,
    MANUAL_COUNT,
    MAX_SENSORS,
    MIDI_CHANNEL_BASE,
    SENSOR_NODE_DEVICE_IDS,
    TOTAL_SENSORS,
)


def board_index(board_id: int) -> int | None:
    try:
        return SENSOR_NODE_DEVICE_IDS.index(board_id)
    except ValueError:
        return None


def board_manual_index(board_id: int) -> int | None:
    board_idx = board_index(board_id)
    if board_idx is None:
        return None
    return MIDI_CHANNEL_BASE + (board_idx // BOARD_PAIR_SIZE)


def build_index_to_channel() -> dict[int, int]:
    """Map global sensor index to MIDI channel (per board pair)."""
    index_to_channel = {}
    for board_id in SENSOR_NODE_DEVICE_IDS:
        board_idx = board_index(board_id)
        if board_idx is None:
            continue
        channel = MIDI_CHANNEL_BASE + (board_idx // BOARD_PAIR_SIZE)
        base = board_idx * MAX_SENSORS
        for sensor_idx in range(MAX_SENSORS):
            global_idx = base + sensor_idx
            if global_idx >= TOTAL_SENSORS:
                continue
            index_to_channel[global_idx] = channel
    return index_to_channel


def build_index_to_midi_low_to_high(
    num_sensors: int,
    low_midi: int,
    high_midi: int,
    *,
    skip_indices=None,
) -> dict[int, int | None]:
    return build_index_to_midi_linear(
        num_sensors=num_sensors,
        bottom_midi=low_midi,
        midi_max=high_midi,
        skip_indices=skip_indices,
        print_table=False,
    )


def build_index_to_midi_by_manual(
    disabled_sensors: set[int],
    *,
    sensors_per_manual: int,
    low_midi: int,
    high_midi: int,
) -> dict[int, int | None]:
    index_to_midi: dict[int, int | None] = {}
    for manual_idx in range(MANUAL_COUNT):
        start = manual_idx * sensors_per_manual
        end = start + sensors_per_manual
        manual_disabled = {idx - start for idx in disabled_sensors if start <= idx < end}
        manual_map = build_index_to_midi_low_to_high(
            sensors_per_manual,
            low_midi,
            high_midi,
            skip_indices=manual_disabled,
        )
        for offset, note in manual_map.items():
            global_idx = start + offset
            if global_idx >= TOTAL_SENSORS:
                continue
            index_to_midi[global_idx] = note
    return index_to_midi


def build_note_maps(index_to_midi: dict[int, int | None], index_to_channel: dict[int, int]):
    note_sensors = {}
    for idx, note in index_to_midi.items():
        if note is None:
            continue
        channel = index_to_channel.get(idx)
        if channel is None:
            continue
        key = (channel, note)
        note_sensors.setdefault(key, []).append(idx)
    note_active = {key: False for key in note_sensors}
    return note_sensors, note_active


def print_midi_mapping(
    index_to_midi: dict[int, int | None],
    logical_index: dict[int, int],
    index_to_channel: dict[int, int] | None = None,
) -> None:
    print("\n# MIDI mapping (sensor -> note)")
    if index_to_channel is None:
        print(
            "{:>4s} | {:>4s} | {:>5s} | {:>6s} | {:>4s} | {:>4s}".format(
                "lidx", "gidx", "board", "sensor", "midi", "note"
            )
        )
        print("-" * 46)
    else:
        print(
            "{:>4s} | {:>4s} | {:>5s} | {:>6s} | {:>2s} | {:>4s} | {:>4s}".format(
                "lidx", "gidx", "board", "sensor", "ch", "midi", "note"
            )
        )
        print("-" * 53)
    for idx in range(TOTAL_SENSORS):
        lidx = logical_index.get(idx)
        lidx_str = "--" if lidx is None else str(lidx)
        board_slot = idx // MAX_SENSORS
        if board_slot < len(SENSOR_NODE_DEVICE_IDS):
            board_id = SENSOR_NODE_DEVICE_IDS[board_slot]
        else:
            board_id = -1
        sensor_id = idx % MAX_SENSORS
        note = index_to_midi.get(idx)
        midi_str = "None" if note is None else str(int(note))
        note_str = midi_to_name(note)
        if index_to_channel is None:
            print(
                "{:>4s} | {:>4d} | {:>5d} | {:>6d} | {:>4s} | {:>4s}".format(
                    lidx_str, idx, board_id, sensor_id, midi_str, note_str
                )
            )
        else:
            channel = index_to_channel.get(idx)
            channel_str = "--" if channel is None else str(channel + 1)
            print(
                "{:>4s} | {:>4d} | {:>5d} | {:>6d} | {:>2s} | {:>4s} | {:>4s}".format(
                    lidx_str, idx, board_id, sensor_id, channel_str, midi_str, note_str
                )
            )


def global_index(board_id: int, sensor_idx: int) -> int | None:
    board_idx = board_index(board_id)
    if board_idx is None:
        return None
    return (board_idx * MAX_SENSORS) + sensor_idx
