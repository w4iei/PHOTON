"""Statistics helpers for the RS-485 sensor node."""

from __future__ import annotations

import math
from array import array

from .constants import DATA_RECORD_BYTES, STD_WINDOW


class StatsTracker:
    def __init__(self, active_sensors: int, window: int = STD_WINDOW):
        self.active_sensors = active_sensors
        self.window = window
        self.history = array("H", [0] * (active_sensors * window))
        self.last_valid = [0] * active_sensors
        self.sum = [0] * active_sensors
        self.sumsq = [0] * active_sensors
        self.index = 0
        self.count = 0

    def update(self, scan_buffer, sensor_disabled) -> None:
        base = self.index * self.active_sensors
        for sensor_idx in range(self.active_sensors):
            value = int(scan_buffer[sensor_idx])
            if sensor_disabled[sensor_idx]:
                value = 0
                self.last_valid[sensor_idx] = 0
            elif value == 0:
                # Zero is invalid for these sensors; keep the rolling
                # statistics on the last known non-zero value.
                value = self.last_valid[sensor_idx]
            else:
                self.last_valid[sensor_idx] = value
            old = self.history[base + sensor_idx]
            self.history[base + sensor_idx] = value
            self.sum[sensor_idx] += value - old
            self.sumsq[sensor_idx] += (value * value) - (old * old)
        if self.count < self.window:
            self.count += 1
        self.index = (self.index + 1) % self.window

    def std_for_sensor(self, sensor_idx: int, sensor_disabled) -> int:
        if self.count <= 1 or sensor_disabled[sensor_idx]:
            return 0
        mean = self.sum[sensor_idx] / self.count
        variance = (self.sumsq[sensor_idx] / self.count) - (mean * mean)
        if variance < 0:
            variance = 0.0
        std_v = int(math.sqrt(variance) + 0.5)
        if std_v < 0:
            std_v = 0
        if std_v > 0xFFFF:
            std_v = 0xFFFF
        return std_v


def refresh_data_payload(data_payload, scan_buffer, sensor_disabled, stats: StatsTracker) -> None:
    for sensor_idx in range(stats.active_sensors):
        if sensor_disabled[sensor_idx]:
            value = 0
            std_v = 0
        else:
            value = int(scan_buffer[sensor_idx])
            std_v = stats.std_for_sensor(sensor_idx, sensor_disabled)
        offset = sensor_idx * DATA_RECORD_BYTES
        data_payload[offset] = value & 0xFF
        data_payload[offset + 1] = (value >> 8) & 0xFF
        data_payload[offset + 2] = std_v & 0xFF
        data_payload[offset + 3] = (std_v >> 8) & 0xFF


class ScanRateTracker:
    def __init__(self, window_s: float):
        self.window_s = window_s
        self.times = []
        self.start_idx = 0

    def update(self, now: float) -> None:
        self.times.append(now)
        cutoff = now - self.window_s
        start = self.start_idx
        total = len(self.times)
        while start < total and self.times[start] < cutoff:
            start += 1
        self.start_idx = start
        # Compact periodically to avoid unbounded growth.
        if self.start_idx > 256 and (self.start_idx * 2) >= len(self.times):
            self.times = self.times[self.start_idx :]
            self.start_idx = 0

    def rate_hz(self, now: float) -> float:
        if self.start_idx >= len(self.times):
            return 0.0
        elapsed = max(now - self.times[self.start_idx], 0.001)
        return (len(self.times) - self.start_idx) / elapsed
