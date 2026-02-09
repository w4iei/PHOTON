"""Event detection and queueing for the RS-485 sensor node."""

from __future__ import annotations

import random
import time

from app.rs485_bus import FRAME_TYPE_EVENT

from .constants import (
    ADJACENT_GUARD_PCT,
    EVENT_STATE_OFF,
    EVENT_STATE_ON,
    POLARITY_NORMAL,
    POLARITY_REVERSED,
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
        min_event_range: int,
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
        self.min_event_range = min_event_range
        self.release_pct = release_pct
        self.activation_pct = activation_pct
        self.log_event_details = log_event_details
        self.event_queue = []
        self.event_in_flight = None
        self.event_seq = 0

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
        self.bus.send_frame(FRAME_TYPE_EVENT, self.device_id, event["payload"], event["seq"])
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

    def process_sample(self, sensor_idx: int, value: int) -> None:
        now = time.monotonic()
        if self.state.sensor_disabled[sensor_idx]:
            self.state.latest_values[sensor_idx] = 0
            return
        self.state.latest_values[sensor_idx] = value

        def _neighbor_is_active_for_guard(neighbor_idx: int) -> bool:
            if neighbor_idx < 0 or neighbor_idx >= self.state.active_sensors:
                return False
            if self.state.sensor_disabled[neighbor_idx]:
                return False
            neighbor_min = self.state.sensor_min[neighbor_idx]
            neighbor_max = self.state.sensor_max[neighbor_idx]
            neighbor_val = self.state.latest_values[neighbor_idx]
            neighbor_low = neighbor_min if neighbor_min < neighbor_val else neighbor_val
            neighbor_high = neighbor_max if neighbor_max > neighbor_val else neighbor_val
            neighbor_rng = neighbor_high - neighbor_low
            if neighbor_rng < self.min_event_range:
                return False
            if self.state.sensor_polarity[neighbor_idx] == POLARITY_REVERSED:
                threshold = neighbor_high - (neighbor_rng * ADJACENT_GUARD_PCT) // 100
                return neighbor_val <= threshold
            threshold = neighbor_low + (neighbor_rng * ADJACENT_GUARD_PCT) // 100
            return neighbor_val >= threshold

        def _should_update_max(idx: int) -> bool:
            rng = self.state.sensor_max[idx] - self.state.sensor_min[idx]
            if rng < self.min_event_range:
                return True
            if _neighbor_is_active_for_guard(idx - 1):
                return False
            if _neighbor_is_active_for_guard(idx + 1):
                return False
            return True

        if value < self.state.sensor_min[sensor_idx]:
            self.state.sensor_min[sensor_idx] = value
        if value > self.state.sensor_max[sensor_idx] and _should_update_max(sensor_idx):
            self.state.sensor_max[sensor_idx] = value
        min_v = self.state.sensor_min[sensor_idx]
        max_v = self.state.sensor_max[sensor_idx]
        rng = max_v - min_v
        if rng < self.min_event_range:
            self.state.sensor_on[sensor_idx] = False
            self.state.sensor_active[sensor_idx] = False
            self.state.sensor_activation_t[sensor_idx] = None
            self.state.sensor_strike_pending[sensor_idx] = False
            self.state.sensor_strike_time[sensor_idx] = 0.0
            self.state.sensor_release_pending[sensor_idx] = False
            self.state.sensor_release_time[sensor_idx] = 0.0
            return

        strike_pct = self.state.sensor_strike_pct[sensor_idx]
        vel_pct = strike_pct + VELOCITY_WINDOW_PCT
        if vel_pct > 100:
            vel_pct = 100
        rel_pct = min(self.release_pct, strike_pct)
        polarity = self.state.sensor_polarity[sensor_idx]
        if polarity == POLARITY_NORMAL:
            activation_thr = self.state.sensor_min[sensor_idx] + (rng * self.activation_pct) // 100
            strike_thr = self.state.sensor_min[sensor_idx] + (rng * strike_pct) // 100
            velocity_thr = self.state.sensor_min[sensor_idx] + (rng * vel_pct) // 100
            release_thr = self.state.sensor_min[sensor_idx] + (rng * rel_pct) // 100
            is_active = value >= activation_thr
        else:
            activation_thr = self.state.sensor_max[sensor_idx] - (rng * self.activation_pct) // 100
            strike_thr = self.state.sensor_max[sensor_idx] - (rng * strike_pct) // 100
            velocity_thr = self.state.sensor_max[sensor_idx] - (rng * vel_pct) // 100
            release_thr = self.state.sensor_max[sensor_idx] - (rng * rel_pct) // 100
            is_active = value <= activation_thr

        if is_active:
            if not self.state.sensor_active[sensor_idx]:
                self.state.sensor_activation_t[sensor_idx] = now
        else:
            if self.state.sensor_active[sensor_idx]:
                self.state.sensor_activation_t[sensor_idx] = None
        self.state.sensor_active[sensor_idx] = is_active

        if is_active and self.trace is not None:
            self.trace.ensure_slot(sensor_idx, now)
        if self.trace is not None:
            self.trace.write_sample(sensor_idx, value, now)

        def _dt_ms(start_t: float, end_t: float) -> int:
            dt_ms = int((end_t - start_t) * 1000.0 + 0.5)
            if dt_ms < 0:
                dt_ms = 0
            if dt_ms > 0xFFFF:
                dt_ms = 0xFFFF
            return dt_ms

        if not self.state.sensor_on[sensor_idx]:
            if not self.state.sensor_strike_pending[sensor_idx]:
                if polarity == POLARITY_NORMAL:
                    if value >= strike_thr:
                        self.state.sensor_strike_pending[sensor_idx] = True
                        self.state.sensor_strike_time[sensor_idx] = now
                else:
                    if value <= strike_thr:
                        self.state.sensor_strike_pending[sensor_idx] = True
                        self.state.sensor_strike_time[sensor_idx] = now
            else:
                if polarity == POLARITY_NORMAL:
                    if value >= velocity_thr:
                        dt_ms = _dt_ms(self.state.sensor_strike_time[sensor_idx], now)
                        if self.log_event_details:
                            print(
                                "[RS485 node %d] EVENT ON sensor=%d val=%d min=%d max=%d rng=%d "
                                "polarity=%d strike=%d%% release=%d%% activation=%d%% "
                                "strike_thr=%d velocity_thr=%d release_thr=%d dt_ms=%d"
                                % (
                                    self.device_id,
                                    sensor_idx,
                                    value,
                                    self.state.sensor_min[sensor_idx],
                                    self.state.sensor_max[sensor_idx],
                                    rng,
                                    polarity,
                                    strike_pct,
                                    self.release_pct,
                                    self.activation_pct,
                                    strike_thr,
                                    velocity_thr,
                                    release_thr,
                                    dt_ms,
                                )
                            )
                        self.queue_event(sensor_idx, EVENT_STATE_ON, dt_ms)
                        self.state.sensor_on[sensor_idx] = True
                        self.state.sensor_strike_pending[sensor_idx] = False
                        self.state.sensor_activation_t[sensor_idx] = None
                        self.state.sensor_release_pending[sensor_idx] = False
                        self.state.sensor_release_time[sensor_idx] = 0.0
                    elif value < strike_thr:
                        self.state.sensor_strike_pending[sensor_idx] = False
                else:
                    if value <= velocity_thr:
                        dt_ms = _dt_ms(self.state.sensor_strike_time[sensor_idx], now)
                        if self.log_event_details:
                            print(
                                "[RS485 node %d] EVENT ON sensor=%d val=%d min=%d max=%d rng=%d "
                                "polarity=%d strike=%d%% release=%d%% activation=%d%% "
                                "strike_thr=%d velocity_thr=%d release_thr=%d dt_ms=%d"
                                % (
                                    self.device_id,
                                    sensor_idx,
                                    value,
                                    self.state.sensor_min[sensor_idx],
                                    self.state.sensor_max[sensor_idx],
                                    rng,
                                    polarity,
                                    strike_pct,
                                    self.release_pct,
                                    self.activation_pct,
                                    strike_thr,
                                    velocity_thr,
                                    release_thr,
                                    dt_ms,
                                )
                            )
                        self.queue_event(sensor_idx, EVENT_STATE_ON, dt_ms)
                        self.state.sensor_on[sensor_idx] = True
                        self.state.sensor_strike_pending[sensor_idx] = False
                        self.state.sensor_activation_t[sensor_idx] = None
                        self.state.sensor_release_pending[sensor_idx] = False
                        self.state.sensor_release_time[sensor_idx] = 0.0
                    elif value > strike_thr:
                        self.state.sensor_strike_pending[sensor_idx] = False
        else:
            if polarity == POLARITY_NORMAL:
                if not self.state.sensor_release_pending[sensor_idx]:
                    if value <= velocity_thr:
                        self.state.sensor_release_pending[sensor_idx] = True
                        self.state.sensor_release_time[sensor_idx] = now
                else:
                    if value <= release_thr:
                        dt_ms = _dt_ms(self.state.sensor_release_time[sensor_idx], now)
                        if self.log_event_details:
                            print(
                                "[RS485 node %d] EVENT OFF sensor=%d val=%d min=%d max=%d rng=%d "
                                "polarity=%d strike=%d%% release=%d%% activation=%d%% "
                                "strike_thr=%d velocity_thr=%d release_thr=%d dt_ms=%d"
                                % (
                                    self.device_id,
                                    sensor_idx,
                                    value,
                                    self.state.sensor_min[sensor_idx],
                                    self.state.sensor_max[sensor_idx],
                                    rng,
                                    polarity,
                                    strike_pct,
                                    self.release_pct,
                                    self.activation_pct,
                                    strike_thr,
                                    velocity_thr,
                                    release_thr,
                                    dt_ms,
                                )
                            )
                        self.queue_event(sensor_idx, EVENT_STATE_OFF, dt_ms)
                        self.state.sensor_on[sensor_idx] = False
                        self.state.sensor_strike_pending[sensor_idx] = False
                        self.state.sensor_release_pending[sensor_idx] = False
                        self.state.sensor_release_time[sensor_idx] = 0.0
                    elif value > velocity_thr:
                        self.state.sensor_release_pending[sensor_idx] = False
            else:
                if not self.state.sensor_release_pending[sensor_idx]:
                    if value >= velocity_thr:
                        self.state.sensor_release_pending[sensor_idx] = True
                        self.state.sensor_release_time[sensor_idx] = now
                else:
                    if value >= release_thr:
                        dt_ms = _dt_ms(self.state.sensor_release_time[sensor_idx], now)
                        if self.log_event_details:
                            print(
                                "[RS485 node %d] EVENT OFF sensor=%d val=%d min=%d max=%d rng=%d "
                                "polarity=%d strike=%d%% release=%d%% activation=%d%% "
                                "strike_thr=%d velocity_thr=%d release_thr=%d dt_ms=%d"
                                % (
                                    self.device_id,
                                    sensor_idx,
                                    value,
                                    self.state.sensor_min[sensor_idx],
                                    self.state.sensor_max[sensor_idx],
                                    rng,
                                    polarity,
                                    strike_pct,
                                    self.release_pct,
                                    self.activation_pct,
                                    strike_thr,
                                    velocity_thr,
                                    release_thr,
                                    dt_ms,
                                )
                            )
                        self.queue_event(sensor_idx, EVENT_STATE_OFF, dt_ms)
                        self.state.sensor_on[sensor_idx] = False
                        self.state.sensor_strike_pending[sensor_idx] = False
                        self.state.sensor_release_pending[sensor_idx] = False
                        self.state.sensor_release_time[sensor_idx] = 0.0
                    elif value < velocity_thr:
                        self.state.sensor_release_pending[sensor_idx] = False
