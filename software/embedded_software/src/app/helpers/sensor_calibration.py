"""Persist per-sensor min/max calibration values for RS485 sensor nodes.

Each node stores the min/max range for every sensor so plucking points can be
reliably mapped across boots. Max values must be frozen once a good range is
learned, because adjacent key activity can drive max upward after calibration.
"""

from __future__ import annotations

import json


def load_calibration_file(path: str) -> dict | None:
    try:
        with open(path, "r") as handle:
            payload = json.load(handle)
    except OSError:
        return None
    except ValueError:
        return None
    if not isinstance(payload, dict):
        return None
    return payload


def save_calibration_file(path: str, payload: dict, debug: list[str] | None = None) -> bool:
    try:
        with open(path, "w") as handle:
            json.dump(payload, handle)
    except Exception as exc:
        if debug is not None:
            debug.append("write %s failed: %s: %s" % (path, exc.__class__.__name__, exc))
        return False
    return True


def build_payload(
    existing: dict | None,
    device_id: int,
    active_sensors: int,
    mins: list[int],
    maxs: list[int],
) -> dict:
    nodes: dict[str, dict] = {}
    if isinstance(existing, dict):
        existing_nodes = existing.get("nodes")
        if isinstance(existing_nodes, dict):
            nodes.update(existing_nodes)
        elif "min" in existing and "max" in existing:
            legacy_id = existing.get("device_id", device_id)
            nodes[str(legacy_id)] = existing
    nodes[str(device_id)] = {
        "device_id": device_id,
        "active_sensors": active_sensors,
        "min": mins,
        "max": maxs,
    }
    return {"version": 1, "nodes": nodes}


def extract_node_calibration(payload: dict, device_id: int) -> dict | None:
    nodes = payload.get("nodes")
    if isinstance(nodes, dict):
        node = nodes.get(str(device_id))
        if isinstance(node, dict):
            return node
        return None
    if "min" in payload and "max" in payload:
        if "device_id" in payload:
            try:
                if int(payload["device_id"]) != device_id:
                    return None
            except Exception:
                return None
        return payload
    return None
