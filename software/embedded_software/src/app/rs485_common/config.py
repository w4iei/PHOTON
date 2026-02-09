"""Shared configuration helpers for RS-485 apps."""

from __future__ import annotations

import json


def deep_merge(base, override):
    if isinstance(base, dict) and isinstance(override, dict):
        merged = dict(base)
        for key, value in override.items():
            merged[key] = deep_merge(base.get(key), value)
        return merged
    if override is None:
        return base
    return override


def load_config(path: str) -> dict:
    try:
        with open(path, "r") as handle:
            return json.load(handle)
    except OSError:
        return {}
    except ValueError as exc:
        raise RuntimeError(f"Invalid JSON in {path}: {exc}") from exc
