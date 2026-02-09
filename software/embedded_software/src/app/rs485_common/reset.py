"""Shared reset helpers for RS-485 apps."""

from __future__ import annotations

try:
    import microcontroller
except Exception:  # pragma: no cover - host envs don't have CircuitPython modules
    microcontroller = None  # type: ignore

try:
    import supervisor
except Exception:  # pragma: no cover - host envs don't have CircuitPython modules
    supervisor = None  # type: ignore


def reset_board() -> bool:
    if microcontroller is not None:
        try:
            microcontroller.reset()
            return True
        except Exception:
            pass
    if supervisor is not None:
        try:
            supervisor.reload()
            return True
        except Exception:
            pass
    return False
