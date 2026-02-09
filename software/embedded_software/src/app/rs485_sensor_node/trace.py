"""Ring-buffered sensor trace capture for the RS-485 node, with chunked readout.

Allocates per-sensor trace slots in RAM, samples at a fixed interval, expires
inactive sensors after a hold time, and formats trace chunks for request/response
delivery over the bus.
"""

from __future__ import annotations

from .constants import TRACE_CHUNK_SAMPLES


class TraceRecorder:
    def __init__(
        self,
        *,
        active_sensors: int,
        trace_slots: int,
        trace_samples: int,
        trace_interval: float,
        trace_hold_s: float,
        trace_chunk_samples: int,
        trace_chunk_count: int,
    ):
        self.active_sensors = active_sensors
        self.trace_slots = trace_slots
        self.trace_samples = trace_samples
        self.trace_interval = trace_interval
        self.trace_hold_s = trace_hold_s
        self.trace_chunk_samples = trace_chunk_samples
        self.trace_chunk_count = trace_chunk_count
        self.trace_buffers = []
        self.sensor_to_slot = [-1] * active_sensors
        self.enabled = (
            trace_slots > 0 and trace_samples > 0 and trace_chunk_samples > 0 and trace_interval > 0
        )
        if self.enabled:
            for _ in range(trace_slots):
                self.trace_buffers.append(
                    {
                        "sensor": None,
                        "buffer": bytearray(trace_samples * 2),
                        "write_idx": 0,
                        "samples_written": 0,
                        "next_sample_at": 0.0,
                        "last_active": 0.0,
                    }
                )
        else:
            self.trace_slots = 0

    @classmethod
    def from_config(
        cls,
        *,
        active_sensors: int,
        trace_slots: int,
        trace_seconds: int,
        trace_sample_hz: int,
        trace_hold_s: float,
        bus_max_payload: int,
    ) -> "TraceRecorder":
        trace_samples = max(0, trace_seconds * trace_sample_hz)
        trace_interval = (1.0 / trace_sample_hz) if trace_sample_hz > 0 else 0.0
        trace_chunk_samples = min(TRACE_CHUNK_SAMPLES, max((bus_max_payload - 4) // 2, 0))
        trace_chunk_count = (
            (trace_samples + trace_chunk_samples - 1) // trace_chunk_samples if trace_chunk_samples else 0
        )
        if trace_slots <= 0 or trace_samples <= 0 or trace_chunk_samples <= 0:
            trace_slots = 0
        return cls(
            active_sensors=active_sensors,
            trace_slots=trace_slots,
            trace_samples=trace_samples,
            trace_interval=trace_interval,
            trace_hold_s=trace_hold_s,
            trace_chunk_samples=trace_chunk_samples,
            trace_chunk_count=trace_chunk_count,
        )

    def ensure_slot(self, sensor_idx: int, now: float) -> None:
        if not self.enabled:
            return
        slot_idx = self.sensor_to_slot[sensor_idx]
        if slot_idx >= 0:
            self.trace_buffers[slot_idx]["last_active"] = now
            return
        for idx, slot in enumerate(self.trace_buffers):
            if slot["sensor"] is None:
                slot["sensor"] = sensor_idx
                slot["write_idx"] = 0
                slot["samples_written"] = 0
                slot["next_sample_at"] = now
                slot["last_active"] = now
                self.sensor_to_slot[sensor_idx] = idx
                break

    def write_sample(self, sensor_idx: int, value: int, now: float) -> None:
        if not self.enabled:
            return
        slot_idx = self.sensor_to_slot[sensor_idx]
        if slot_idx < 0:
            return
        slot = self.trace_buffers[slot_idx]
        if now < slot["next_sample_at"]:
            return
        offset = slot["write_idx"] * 2
        slot["buffer"][offset : offset + 2] = int(value).to_bytes(2, "little")
        slot["write_idx"] = (slot["write_idx"] + 1) % self.trace_samples
        if slot["samples_written"] < self.trace_samples:
            slot["samples_written"] += 1
        slot["next_sample_at"] = now + self.trace_interval

    def cleanup(self, now: float) -> None:
        if not self.enabled:
            return
        for idx, slot in enumerate(self.trace_buffers):
            sensor_idx = slot["sensor"]
            if sensor_idx is None:
                continue
            if (now - slot["last_active"]) > self.trace_hold_s:
                slot["sensor"] = None
                slot["samples_written"] = 0
                slot["write_idx"] = 0
                slot["next_sample_at"] = 0.0
                self.sensor_to_slot[sensor_idx] = -1

    def build_trace_response(self, sensor_idx: int, chunk_idx: int) -> tuple[bytearray, int] | None:
        if not self.enabled:
            return None
        if sensor_idx >= self.active_sensors or chunk_idx >= self.trace_chunk_count:
            return None
        resp = bytearray(4 + (self.trace_chunk_samples * 2))
        resp[0] = sensor_idx
        resp[1] = chunk_idx
        valid_samples = 0
        slot_idx = self.sensor_to_slot[sensor_idx]
        if slot_idx >= 0:
            slot = self.trace_buffers[slot_idx]
            valid_samples = min(slot["samples_written"], self.trace_samples)
            filled = slot["samples_written"] >= self.trace_samples
            start_sample = chunk_idx * self.trace_chunk_samples
            for i in range(self.trace_chunk_samples):
                sample_idx = start_sample + i
                if sample_idx >= self.trace_samples:
                    break
                if not filled:
                    if sample_idx >= slot["samples_written"]:
                        break
                    src_idx = sample_idx
                else:
                    src_idx = (slot["write_idx"] + sample_idx) % self.trace_samples
                src_offset = src_idx * 2
                dst_offset = 4 + (i * 2)
                resp[dst_offset : dst_offset + 2] = slot["buffer"][src_offset : src_offset + 2]
        return resp, valid_samples
