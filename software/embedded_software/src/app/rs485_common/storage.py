"""Shared storage helpers for RS-485 apps."""

from __future__ import annotations

try:
    import storage
except Exception:  # pragma: no cover - host envs don't have CircuitPython modules
    storage = None  # type: ignore


def get_root_readonly() -> bool | None:
    if storage is None:
        return None
    getter = getattr(storage, "getmount", None)
    if getter is None:
        return None
    try:
        mount = getter("/")
    except Exception:
        return None
    return bool(getattr(mount, "readonly", False))


def remount_root(readonly: bool, debug: list[str] | None = None) -> bool:
    if storage is None:
        if debug is not None:
            debug.append("storage module unavailable")
        return False
    try:
        storage.remount("/", readonly=readonly)
    except Exception as exc:
        if debug is not None:
            debug.append(
                "remount(readonly=%s) failed: %s: %s" % (readonly, exc.__class__.__name__, exc)
            )
        return False
    return True
