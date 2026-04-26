"""Runtime state containers for the RS-485 sensor node."""

from __future__ import annotations

from array import array

from .constants import DATA_RECORD_BYTES, POLARITY_NORMAL, SENSOR_VALUE_MAX


class NodeState:
    def __init__(self, active_sensors: int, strike_pct_default: int, scan_buffer=None):
        self.active_sensors = active_sensors
        self.latest_values = array("H", [0] * active_sensors)
        if scan_buffer is None:
            self.scan_buffer = array("H", [0] * active_sensors)
        else:
            if len(scan_buffer) < active_sensors:
                raise ValueError("scan_buffer must be at least active_sensors long")
            self.scan_buffer = scan_buffer
        self.sensor_min = array("H", [SENSOR_VALUE_MAX] * active_sensors)
        self.sensor_max = array("H", [0] * active_sensors)
        self.sensor_polarity = array("B", [POLARITY_NORMAL] * active_sensors)
        self.sensor_strike_pct = array("B", [strike_pct_default] * active_sensors)
        self.sensor_on = array("B", [0] * active_sensors)
        self.sensor_active = array("B", [0] * active_sensors)
        self.sensor_strike_pending = array("B", [0] * active_sensors)
        self.sensor_release_pending = array("B", [0] * active_sensors)
        self.sensor_strike_time_ms = array("I", [0] * active_sensors)
        self.sensor_release_time_ms = array("I", [0] * active_sensors)
        self.sensor_baseline = [0] * active_sensors
        self.sensor_disabled = array("B", [0] * active_sensors)
        self.data_payload = bytearray(active_sensors * DATA_RECORD_BYTES)
