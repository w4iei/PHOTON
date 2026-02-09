"""
Lightweight configuration loader for board-specific settings.

Pin assignments live in ``/config/pins.json`` so that developers can tweak
hardware wiring without touching the Python sources.  The loader merges that
JSON file with baked-in defaults and resolves symbolic names (``"GP24"``,
``"A0"``, ``"LED"`` …) into the actual ``board`` pin objects.
"""

from __future__ import annotations

import json

try:
    import board  # type: ignore
except ImportError:  # pragma: no cover - host environments don't ship CircuitPython
    board = None  # type: ignore

_CONFIG_PATH = "/config/pins.json"

_DEFAULT_PIN_CONFIG = {
    "sensors": {
        "bank": {
            "a": "GP24",
            "b": "GP25",
        },
        "select": {
            "lsb": "GP19",
            "mid": "GP20",
            "msb": "GP21",
        },
        "adc": ["A0", "A1", "A2", "A3"],
    },
    "uart": {
        "bottom": {"tx": "GP4", "rx": "GP5"},
        "right": {"tx": "GP22", "rx": "GP23"},
        "left": {"tx": "GP10", "rx": "GP11"},
    },
    "i2c": {
        "default": {"sda": "GP6", "scl": "GP7"},
    },
    "leds": {
        "status_green": "GP1",
        "status_red": "GP2",
    },
}

_PIN_CACHE = None


def _deep_merge(base, override):
    if isinstance(base, dict) and isinstance(override, dict):
        merged = dict(base)
        for key, value in override.items():
            merged[key] = _deep_merge(base.get(key), value)
        return merged
    if override is None:
        return base
    return override


def _looks_like_pin_name(name):
    cleaned = name.strip().upper().replace(" ", "")
    if cleaned in ("LED", "NEOPIXEL"):
        return True
    if cleaned.startswith("GPIO"):
        return True
    if cleaned.startswith("GP") and cleaned[2:].isdigit():
        return True
    if cleaned.startswith("A") and cleaned[1:].isdigit():
        return True
    return False


def _normalise_pin_name(name):
    cleaned = name.strip().upper().replace(" ", "")
    if cleaned.startswith("GPIO"):
        cleaned = "GP" + cleaned[4:]
    return cleaned


def _resolve_pin(name):
    if board is None:
        raise RuntimeError("Pin resolution requires CircuitPython's 'board' module.")

    normalised = _normalise_pin_name(name)
    try:
        return getattr(board, normalised)
    except AttributeError as exc:
        raise ValueError(f"Unknown board pin '{name}' (normalised to '{normalised}')") from exc


def _materialise_pins(obj):
    if isinstance(obj, dict):
        return {k: _materialise_pins(v) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_materialise_pins(v) for v in obj]
    if isinstance(obj, str) and _looks_like_pin_name(obj):
        return _resolve_pin(obj)
    return obj


def _load_json_config(path):
    try:
        with open(path, "r") as fp:
            return json.load(fp)
    except OSError:
        return {}
    except ValueError as exc:
        raise RuntimeError(f"Invalid JSON in {path}: {exc}") from exc


def pins():
    """
    Return the resolved pin configuration.

    The result is cached after the first call. Reset ``_PIN_CACHE`` to force a
    reload (handy when editing ``pins.json`` over the REPL).
    """

    global _PIN_CACHE
    if _PIN_CACHE is None:
        if board is None:
            raise RuntimeError("Pin configuration requires CircuitPython's 'board' module.")
        merged = _deep_merge(_DEFAULT_PIN_CONFIG, _load_json_config(_CONFIG_PATH))
        _PIN_CACHE = _materialise_pins(merged)
    return _PIN_CACHE


def reload_pins():
    """Force the configuration to be re-read from disk."""

    global _PIN_CACHE
    _PIN_CACHE = None
    return pins()
