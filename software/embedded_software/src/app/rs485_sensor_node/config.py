"""Configuration loading and merging."""

from __future__ import annotations

from app.rs485_common.config import deep_merge, load_config


def parse_disabled_sensors(value, total: int) -> set[int]:
    disabled = set()
    if isinstance(value, (list, tuple)):
        for entry in value:
            try:
                idx = int(entry)
            except Exception:
                continue
            if 0 <= idx < total:
                disabled.add(idx)
    return disabled
