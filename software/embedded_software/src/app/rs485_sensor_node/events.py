"""Event detection and queueing for the RS-485 sensor node."""

from __future__ import annotations

from array import array
import random
import time

from photon_rs485 import FRAME_TYPE_EVENT

from .constants import (
    EVENT_STATE_OFF,
    EVENT_STATE_ON,
    VELOCITY_WINDOW_PCT,
)


def build_event_payload(sensor_idx: int, state: int, dt_ms: int, seq: int, device_id: int) -> bytes:
    payload = bytearray(7)
    payload[0] = sensor_idx & 0xFF
    payload[1] = EVENT_STATE_ON if state else EVENT_STATE_OFF
    if dt_ms < 0:
        dt_ms = 0
    if dt_ms > 0xFFFF:
        dt_ms = 0xFFFF
    payload[2:4] = int(dt_ms).to_bytes(2, "little")
    payload[4:6] = (seq & 0xFFFF).to_bytes(2, "little")
    payload[6] = device_id & 0xFF
    return payload


class EventEngine:
    def __init__(
        self,
        *,
        bus,
        device_id: int,
        state,
        counters: dict,
        trace=None,
        event_ack_timeout_s: float,
        event_retry_max: int,
        event_backoff_us,
        event_queue_max: int,
        min_sensor_dynamic_range: int,
        release_pct: int,
        activation_pct: int,
        log_event_details: bool,
    ):
        self.bus = bus
        self.device_id = device_id
        self.state = state
        self.counters = counters
        self.trace = trace
        self.event_ack_timeout_s = event_ack_timeout_s
        self.event_retry_max = event_retry_max
        self.event_backoff_us = event_backoff_us
        self.event_queue_max = event_queue_max
        self.min_sensor_dynamic_range = min_sensor_dynamic_range
        self.release_pct = release_pct
        self.activation_pct = activation_pct
        self.log_event_details = log_event_details
        self.event_queue = []
        self.event_in_flight = None
        self.event_seq = 0
        self._c_process_scan_events = None
        self._c_event_words = None
        self._active_sensors = int(getattr(self.state, "active_sensors", 0))
        if self._active_sensors <= 0:
            raise ValueError("state.active_sensors must be >= 1")
        self._init_c_fast_path()
        self._validate_state_buffers_for_c(self._active_sensors)
        print("Event engine: photon_sensorscan.process_scan_events (C-only)")

    def _init_c_fast_path(self) -> None:
        try:
            module = __import__("photon_sensorscan")
        except Exception as exc:
            raise RuntimeError(
                "Missing photon_sensorscan C event backend; Python event fallback has been removed."
            ) from exc
        process_scan_events = getattr(module, "process_scan_events", None)
        if not callable(process_scan_events):
            raise RuntimeError(
                "photon_sensorscan.process_scan_events is unavailable; Python event fallback has been removed."
            )
        self._c_process_scan_events = process_scan_events
        self._c_event_words = array("H", [0] * max(3, self.event_queue_max * 3))

    def _validate_state_buffers_for_c(self, active: int) -> None:
        required = (
            "latest_values",
            "sensor_disabled",
            "sensor_min",
            "sensor_max",
            "sensor_polarity",
            "sensor_strike_pct",
            "sensor_on",
            "sensor_active",
            "sensor_strike_pending",
            "sensor_strike_time_ms",
            "sensor_release_pending",
            "sensor_release_time_ms",
        )
        for name in required:
            buf = getattr(self.state, name, None)
            if buf is None:
                raise RuntimeError(f"state.{name} is required for C event processing")
            try:
                size = len(buf)
            except Exception as exc:
                raise TypeError(f"state.{name} must be a sized writable buffer") from exc
            if size < active:
                raise ValueError(f"state.{name} length must be >= active_sensors ({active})")

    def _process_scan_c(self, active_sensors: int) -> None:
        if self._c_process_scan_events is None or self._c_event_words is None:
            raise RuntimeError("C event backend is not initialized")
        try:
            event_count = int(
                self._c_process_scan_events(
                    active_sensors=active_sensors,
                    latest_values=self.state.latest_values,
                    sensor_disabled=self.state.sensor_disabled,
                    sensor_min=self.state.sensor_min,
                    sensor_max=self.state.sensor_max,
                    sensor_polarity=self.state.sensor_polarity,
                    sensor_strike_pct=self.state.sensor_strike_pct,
                    sensor_on=self.state.sensor_on,
                    sensor_active=self.state.sensor_active,
                    sensor_strike_pending=self.state.sensor_strike_pending,
                    sensor_strike_time_ms=self.state.sensor_strike_time_ms,
                    sensor_release_pending=self.state.sensor_release_pending,
                    sensor_release_time_ms=self.state.sensor_release_time_ms,
                    min_event_range=self.min_sensor_dynamic_range,
                    release_pct=self.release_pct,
                    activation_pct=self.activation_pct,
                    velocity_window_pct=VELOCITY_WINDOW_PCT,
                    strike_window_pct=30,
                    event_words=self._c_event_words,
                )
            )
        except Exception as exc:
            raise RuntimeError(f"photon_sensorscan.process_scan_events failed: {exc}") from exc
        max_events = len(self._c_event_words) // 3
        if event_count > max_events:
            event_count = max_events
        for event_idx in range(event_count):
            base = event_idx * 3
            sensor_idx = int(self._c_event_words[base])
            state = int(self._c_event_words[base + 1])
            dt_ms = int(self._c_event_words[base + 2])
            if self.log_event_details:
                print(
                    "[RS485 node %d] EVENT %s sensor=%d dt_ms=%d"
                    % (self.device_id, "ON" if state else "OFF", sensor_idx, dt_ms)
                )
            self.queue_event(sensor_idx, state, dt_ms)

    def _backoff_s(self) -> float:
        low = 50
        high = 200
        if isinstance(self.event_backoff_us, (list, tuple)) and len(self.event_backoff_us) >= 2:
            low = int(self.event_backoff_us[0])
            high = int(self.event_backoff_us[1])
        elif isinstance(self.event_backoff_us, (int, float)):
            low = int(self.event_backoff_us)
            high = int(self.event_backoff_us)
        if high < low:
            low, high = high, low
        return random.randint(low, high) / 1_000_000.0

    def queue_event(self, sensor_idx: int, state: int, dt_ms: int) -> None:
        if len(self.event_queue) >= self.event_queue_max:
            self.counters["event_drop"] += 1
            return
        seq = self.event_seq & 0xFFFF
        self.event_seq = (self.event_seq + 1) & 0xFFFF
        payload = build_event_payload(sensor_idx, state, dt_ms, seq, self.device_id)
        self.event_queue.append(
            {"seq": seq, "payload": payload, "sent_at": 0.0, "attempts": 0, "deadline": 0.0}
        )

    def _send_event(self, event, now: float) -> None:
        self.bus.send_frame(FRAME_TYPE_EVENT, self.device_id, event["payload"], event["seq"], ack_timeout_us=0)
        event["sent_at"] = now
        event["attempts"] += 1
        event["deadline"] = now + self.event_ack_timeout_s + self._backoff_s()
        self.counters["event_tx"] += 1

    def service(self, now: float) -> None:
        if self.event_in_flight is None:
            if self.event_queue:
                self.event_in_flight = self.event_queue.pop(0)
                self._send_event(self.event_in_flight, now)
        elif now >= self.event_in_flight["deadline"]:
            if self.event_in_flight["attempts"] >= self.event_retry_max:
                self.event_in_flight = None
            else:
                self.counters["event_retry"] += 1
                self._send_event(self.event_in_flight, now)

    def ack(self, seq: int) -> None:
        if self.event_in_flight and seq == self.event_in_flight["seq"]:
            self.event_in_flight = None

    def process_scan(self, readings, count: int) -> None:
        limit = int(count)
        if limit < 0:
            return
        if limit > self.state.active_sensors:
            limit = self.state.active_sensors
        if limit <= 0:
            return
        self._process_scan_c(limit)

        if self.trace is not None and getattr(self.trace, "enabled", False):
            now = time.monotonic()
            for sensor_idx in range(limit):
                value = int(readings[sensor_idx])
                if self.state.sensor_active[sensor_idx]:
                    self.trace.ensure_slot(sensor_idx, now)
                self.trace.write_sample(sensor_idx, value, now)
