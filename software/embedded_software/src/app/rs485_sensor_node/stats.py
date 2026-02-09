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
        data_payload[offset : offset + 2] = value.to_bytes(2, "little")
        data_payload[offset + 2 : offset + 4] = std_v.to_bytes(2, "little")


class ScanRateTracker:
    def __init__(self, window_s: float):
        self.window_s = window_s
        self.times = []

    def update(self, now: float) -> None:
        self.times.append(now)
        cutoff = now - self.window_s
        while self.times and self.times[0] < cutoff:
            self.times.pop(0)

    def rate_hz(self, now: float) -> float:
        if not self.times:
            return 0.0
        elapsed = max(now - self.times[0], 0.001)
        return len(self.times) / elapsed
