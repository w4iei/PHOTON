"""Shared USB serial helpers for RS-485 apps."""

from __future__ import annotations

import sys

try:
    import supervisor
except Exception:  # pragma: no cover - host envs don't have CircuitPython modules
    supervisor = None  # type: ignore


def serial_poll_requested() -> bool:
    if supervisor is None:
        return False
    try:
        return supervisor.runtime.serial_bytes_available > 0
    except Exception:
        return False


def read_serial_input() -> str:
    if supervisor is None:
        return ""
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
        try:
            return data.decode("utf-8", "ignore")
        except Exception:
            return ""
    return str(data)


def usb_connected() -> bool:
    if supervisor is None:
        return True
    try:
        return supervisor.runtime.usb_connected
    except Exception:
        return True
