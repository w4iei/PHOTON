"""Common helpers shared across the CircuitPython scripts."""

import sys
import time

import supervisor
from ulab import numpy as np

# ---------- Logging ----------
LOG_LEVEL = 2  # 0=off, 1=info, 2=debug
_usb_logging_force_off = False


def usb_logging_active() -> bool:
    """Return True only when USB serial is connected and logging is not force-disabled."""
    if _usb_logging_force_off:
        return False
    try:
        return bool(supervisor.runtime.serial_connected)
    except AttributeError:
        return False


def toggle_usb_logging() -> bool:
    """Toggle the USB logging force-off flag. Returns the new logging-active state."""
    global _usb_logging_force_off
    _usb_logging_force_off = not _usb_logging_force_off
    state = "ENABLED" if not _usb_logging_force_off else "DISABLED"
    # Always print this specific message so the user sees the toggle feedback
    print(f"*** USB logging {state} ***")
    return not _usb_logging_force_off


def safe_print(*args, **kwargs):
    """Print only when USB logging is active."""
    if usb_logging_active():
        print(*args, **kwargs)


def log_info(msg):
    """Emit an informational log line that hosts can parse."""
    if LOG_LEVEL >= 1 and usb_logging_active():
        print(f"# LOG {msg}")

def log_debug(msg):
    """Emit a debug log line (same prefix for compatibility with host scripts)."""
    if LOG_LEVEL >= 2 and usb_logging_active():
        print(f"# LOG {msg}")

def log_note(msg):
    """Emit note-level diagnostics; distinct prefix so host tools can filter."""
    if LOG_LEVEL >= 2 and usb_logging_active():
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

# ---------- USB Serial ----------
def serial_poll_requested() -> bool:
    try:
        return supervisor.runtime.serial_bytes_available > 0
    except Exception:
        return False


def read_serial_input() -> str:
    try:
        count = supervisor.runtime.serial_bytes_available
    except Exception:
        return ""
    if count <= 0:
        return ""
    try:
        data = sys.stdin.read(count)
    except Exception:
        return ""
    if data is None:
        return ""
    if isinstance(data, bytes):
        return data.decode("utf-8", "ignore")
    return str(data)


def print_box(lines: list[str]) -> None:
    """Print lines inside a +-framed box."""
    frame_width = max(len(line) for line in lines)
    print("+-" + ("-" * frame_width) + "-+")
    for line in lines:
        print("| " + line + (" " * (frame_width - len(line))) + " |")
    print("+-" + ("-" * frame_width) + "-+")


def print_disabled_sensor_warning(disabled_count: int) -> None:
    if disabled_count <= 0:
        return
    print_box([
        f"WARNING: {disabled_count} sensor(s) disabled.",
        "Disabled due to high level at boot OR system config.",
        "Please be sure that no keys are pressed",
        "when powering on the photon system.",
        "Config source: app/rs485_system_config.py",
    ])


def usb_connected() -> bool:
    runtime = supervisor.runtime
    return bool(
        getattr(runtime, "usb_connected", False)
        or getattr(runtime, "serial_connected", False)
    )


def usb_console_connected() -> bool:
    runtime = supervisor.runtime
    return bool(
        getattr(runtime, "usb_connected", False)
        or getattr(runtime, "serial_connected", False)
        or getattr(runtime, "serial_bytes_available", 0) > 0
    )
