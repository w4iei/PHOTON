"""Runtime state containers for the RS-485 sensor node."""

from __future__ import annotations

from array import array

from .constants import DATA_RECORD_BYTES, POLARITY_NORMAL, SENSOR_VALUE_MAX


class NodeState:
    def __init__(self, active_sensors: int, strike_pct_default: int):
        self.active_sensors = active_sensors
        self.latest_values = [0] * active_sensors
        self.scan_buffer = array("H", [0] * active_sensors)
        self.sensor_min = [SENSOR_VALUE_MAX] * active_sensors
        self.sensor_max = [0] * active_sensors
        self.sensor_polarity = [POLARITY_NORMAL] * active_sensors
        self.sensor_strike_pct = [strike_pct_default] * active_sensors
        self.sensor_on = [False] * active_sensors
        self.sensor_active = [False] * active_sensors
        self.sensor_activation_t = [None] * active_sensors
        self.sensor_strike_pending = [False] * active_sensors
        self.sensor_strike_time = [0.0] * active_sensors
        self.sensor_release_pending = [False] * active_sensors
        self.sensor_release_time = [0.0] * active_sensors
        self.sensor_to_slot = [-1] * active_sensors
        self.sensor_baseline = [0] * active_sensors
        self.sensor_disabled = [False] * active_sensors
        self.data_payload = bytearray(active_sensors * DATA_RECORD_BYTES)
