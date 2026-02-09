"""Configuration helpers for the RS-485 main host."""

from __future__ import annotations

from .constants import DEFAULT_DISABLED_SENSORS, TOTAL_SENSORS


def parse_disabled_sensors(value) -> set[int]:
    disabled = set(DEFAULT_DISABLED_SENSORS)
    if isinstance(value, (list, tuple)):
        for entry in value:
            try:
                idx = int(entry)
            except Exception:
                continue
            if 0 <= idx < TOTAL_SENSORS:
                disabled.add(idx)
    return disabled
