"""Helpers for USB MIDI transport and board geometry mapping."""

import usb_midi
from adafruit_midi import MIDI  # type: ignore

try:
    from adafruit_midi.note_on import NoteOn
    from adafruit_midi.note_off import NoteOff
except ImportError:
    from adafruit_midi import NoteOn, NoteOff  # type: ignore

from app.helpers.utils import midi_to_name, log_note

# MIDI config
MIDI_CHANNEL = 0
VELOCITY_ON = 100
VELOCITY_OFF = 0

midi = None
midi_out = None

def midi_setup():
    """Initialise the MIDI peripheral, picking the user-facing USB port."""
    global midi, midi_out
    midi_out = usb_midi.ports[1] if len(usb_midi.ports) > 1 else usb_midi.ports[0]
    midi = MIDI(midi_out=midi_out, out_channel=MIDI_CHANNEL)


def _clamp_velocity(value: int, default: int) -> int:
    try:
        value = int(value)
    except Exception:
        return default
    if value < 0:
        return 0
    if value > 127:
        return 127
    return value


def _send_raw_note(status: int, note: int, velocity: int, channel: int) -> None:
    if midi_out is None:
        return
    velocity = _clamp_velocity(velocity, VELOCITY_ON if status == 0x90 else VELOCITY_OFF)
    status_byte = (status & 0xF0) | (channel & 0x0F)
    payload = bytes([status_byte, note & 0x7F, velocity & 0x7F])
    midi_out.write(payload)

def midi_note_on(note: int, *, channel: int | None = None, velocity: int | None = None):
    """Send a MIDI Note On message if the stack is ready."""
    if velocity is None:
        velocity = VELOCITY_ON
    velocity = _clamp_velocity(velocity, VELOCITY_ON)
    if channel is None:
        if midi:
            midi.send(NoteOn(note, velocity))
        return
    _send_raw_note(0x90, note, velocity, channel)

def midi_note_off(note: int, *, channel: int | None = None, velocity: int | None = None):
    """Send a MIDI Note Off message if the stack is ready."""
    if velocity is None:
        velocity = VELOCITY_OFF
    velocity = _clamp_velocity(velocity, VELOCITY_OFF)
    if channel is None:
        if midi:
            midi.send(NoteOff(note, velocity))
        return
    _send_raw_note(0x80, note, velocity, channel)

def build_index_to_midi_range(
    num_sensors: int,
    start_midi: int,
    end_midi: int,
    *,
    midi_min: int = 0,
    midi_max: int = 127,
    skip_indices=None,
    print_table: bool = False,
):
    """Build a linear mapping across a MIDI range (duplicates allowed)."""
    index_to_midi = {}
    skip = set(skip_indices) if skip_indices is not None else set()
    span = max(num_sensors - 1, 1)
    step = (end_midi - start_midi) / span
    for i in range(num_sensors):
        if i in skip:
            index_to_midi[i] = None
            continue
        note = int(round(start_midi + (i * step)))
        if note < midi_min:
            note = midi_min
        if note > midi_max:
            note = midi_max
        index_to_midi[i] = note
    if print_table:
        print("\nIndex → MIDI mapping (range)\nidx | midi | note")
        print("-" * 30)
        for i in range(num_sensors):
            m = index_to_midi[i]
            print("{:3d} | {:4d} | {}".format(i, m, midi_to_name(m)))
    return index_to_midi


def build_index_to_midi_linear(
    num_sensors: int = 64,
    bottom_midi: int = 29,
    *,
    midi_min: int = 0,
    midi_max: int = 127,
    skip_indices=None,
    print_table: bool = False,
):
    """Build a simple linear mapping: index 0 -> bottom_midi, each index +1."""
    index_to_midi = {}
    skip = set(skip_indices) if skip_indices is not None else set()
    note = bottom_midi
    for i in range(num_sensors):
        if i in skip:
            index_to_midi[i] = None
            continue
        if note < midi_min:
            note = midi_min
        if note > midi_max:
            note = midi_max
        index_to_midi[i] = note
        note += 1
    if print_table:
        print("\nIndex → MIDI mapping (linear)\nidx | midi | note")
        print("-" * 30)
        for i in range(num_sensors):
            m = index_to_midi[i]
            print("{:3d} | {:4d} | {}".format(i, m, midi_to_name(m)))
    return index_to_midi

def debug_note_transition(note: int, on: bool, payload: str):
    """Emit a structured log when a note boundary is crossed."""
    state = "ON" if on else "OFF"
    log_note(f"{state} note={note}({midi_to_name(note)}) {payload}")
