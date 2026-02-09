"""Common helpers shared across the CircuitPython scripts."""

import time
from ulab import numpy as np

# ---------- Logging ----------
LOG_LEVEL = 2  # 0=off, 1=info, 2=debug

def log_info(msg):
    """Emit an informational log line that hosts can parse."""
    if LOG_LEVEL >= 1:
        print(f"# LOG {msg}")

def log_debug(msg):
    """Emit a debug log line (same prefix for compatibility with host scripts)."""
    if LOG_LEVEL >= 2:
        print(f"# LOG {msg}")

def log_note(msg):
    """Emit note-level diagnostics; distinct prefix so host tools can filter."""
    if LOG_LEVEL >= 2:
        print(f"# NOTE {msg}")

# ---------- Timing ----------
def sleep_us(us: int):
    """
    Busy-wait for the requested number of microseconds.

    CircuitPython omits ``time.sleep_us`` on some boards, so we create a
    lightweight replacement that is accurate enough for analog settling.
    """
    if hasattr(time, "sleep_us"):
        time.sleep_us(us)
        return
    start = time.monotonic_ns()
    target = start + (us * 1000)
    while time.monotonic_ns() < target:
        pass

# ---------- Note helpers ----------
_NOTE_NAMES_SHARP = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
_NOTE_NAMES_FLAT  = ("C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B")

def midi_to_name(m, prefer_sharps: bool = True) -> str:
    """
    Convert a MIDI note number (0..127) into a note name like ``C4``.

    Args:
        m: MIDI note number or ``None``.
        prefer_sharps: When False, return flats (``Db``) instead of sharps.

    Returns:
        A string representation.  ``None`` or out-of-range values are returned
        as ``"None"`` / ``str(m)`` so that logs stay readable.
    """
    if m is None:
        return "None"
    try:
        m_int = int(m)
    except Exception:
        return str(m)

    if m_int < 0 or m_int > 127:
        return str(m_int)

    names = _NOTE_NAMES_SHARP if prefer_sharps else _NOTE_NAMES_FLAT
    name = names[m_int % 12]
    octave = (m_int // 12) - 1
    return f"{name}{octave}"

def log2_ulab(x):
    """ulab-compatible log2 helper."""
    return np.log(x) / np.log(2.0)

def clamp(v, lo, hi):
    """Clamp ``v`` into the inclusive range [``lo``, ``hi``]."""
    return lo if v < lo else hi if v > hi else v
