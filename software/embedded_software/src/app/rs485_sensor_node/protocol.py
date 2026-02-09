"""RS-485 frame handling for the sensor node."""

from __future__ import annotations

import time

from app.helpers import nvm_flags
from app.rs485_bus import (
    FRAME_TYPE_CAL_ACK,
    FRAME_TYPE_CAL_CMD,
    FRAME_TYPE_DATA_REQ,
    FRAME_TYPE_DATA_RESP,
    FRAME_TYPE_EVENT_ACK,
    FRAME_TYPE_MINMAX_REQ,
    FRAME_TYPE_MINMAX_RESP,
    FRAME_TYPE_PING,
    FRAME_TYPE_PONG,
    FRAME_TYPE_STATS_REQ,
    FRAME_TYPE_STATS_RESP,
    FRAME_TYPE_TRACE_REQ,
    FRAME_TYPE_TRACE_RESP,
)

from .constants import CAL_ACK_FAIL, CAL_ACK_OK, CAL_CMD_RESET, CAL_CMD_SAVE, MINMAX_RECORD_BYTES
from .runtime import reset_board


class FrameDispatcher:
    def __init__(
        self,
        *,
        bus,
        device_id: int,
        active_sensors: int,
        state,
        stats,
        calibration,
        trace,
        event_engine,
        counters: dict,
        scan_rate_tracker,
        auto_reply_ping: bool,
        auto_reply_data: bool,
        bus_max_payload: int,
    ):
        self.bus = bus
        self.device_id = device_id
        self.active_sensors = active_sensors
        self.state = state
        self.stats = stats
        self.calibration = calibration
        self.trace = trace
        self.event_engine = event_engine
        self.counters = counters
        self.scan_rate_tracker = scan_rate_tracker
        self.auto_reply_ping = auto_reply_ping
        self.auto_reply_data = auto_reply_data
        self.bus_max_payload = bus_max_payload

    def send_minmax_dump(self, seq: int) -> int:
        max_count = max((self.bus_max_payload - 2) // MINMAX_RECORD_BYTES, 0)
        if max_count <= 0:
            return seq
        start = 0
        while start < self.active_sensors:
            count = min(max_count, self.active_sensors - start)
            resp = bytearray(2 + (count * MINMAX_RECORD_BYTES))
            resp[0] = start
            resp[1] = count
            for idx in range(count):
                sensor_idx = start + idx
                offset = 2 + (idx * MINMAX_RECORD_BYTES)
                resp[offset : offset + 2] = int(self.state.sensor_min[sensor_idx]).to_bytes(2, "little")
                resp[offset + 2 : offset + 4] = int(self.state.sensor_max[sensor_idx]).to_bytes(2, "little")
                resp[offset + 4 : offset + 6] = int(
                    self.stats.std_for_sensor(sensor_idx, self.state.sensor_disabled)
                ).to_bytes(2, "little")
            self.bus.send_frame(FRAME_TYPE_MINMAX_RESP, self.device_id, resp, seq & 0xFFFF)
            seq = (seq + 1) & 0xFFFF
            start += count
        return seq

    def handle_frames(self) -> None:
        for frame_type, target_id, payload, seq in self.bus.read_frames():
            if target_id not in (self.device_id, 0):
                continue
            self.counters["rx"] += 1
            if frame_type == FRAME_TYPE_PING:
                if not self.auto_reply_ping:
                    self.bus.send_frame(FRAME_TYPE_PONG, self.device_id, b"", seq)
                self.counters["resp"] += 1
            elif frame_type == FRAME_TYPE_DATA_REQ:
                if not self.auto_reply_data:
                    self.bus.send_frame(FRAME_TYPE_DATA_RESP, self.device_id, self.state.data_payload, seq)
                self.counters["resp"] += 1
            elif frame_type == FRAME_TYPE_STATS_REQ:
                now = time.monotonic()
                rate_hz = self.scan_rate_tracker.rate_hz(now)
                rate_tenths = int(rate_hz * 10 + 0.5)
                resp_payload = bytearray(8)
                resp_payload[0:2] = rate_tenths.to_bytes(2, "little")
                resp_payload[2:4] = (self.counters["event_tx"] & 0xFFFF).to_bytes(2, "little")
                resp_payload[4:6] = (self.counters["event_drop"] & 0xFFFF).to_bytes(2, "little")
                resp_payload[6:8] = (self.counters["event_retry"] & 0xFFFF).to_bytes(2, "little")
                self.bus.send_frame(FRAME_TYPE_STATS_RESP, self.device_id, resp_payload, seq)
                self.counters["resp"] += 1
            elif frame_type == FRAME_TYPE_CAL_CMD:
                if not payload:
                    continue
                cmd = payload[0]
                if cmd == CAL_CMD_SAVE:
                    self.send_minmax_dump((seq + 1) & 0xFFFF)
                    saved = self.calibration.save_calibration_payload()
                    flags_ok = True
                    if saved:
                        flags_ok &= nvm_flags.set_usb_drive_disabled(False)
                        flags_ok &= nvm_flags.set_reset_calibration_on_boot(False)
                    status = CAL_ACK_OK if (saved and flags_ok) else CAL_ACK_FAIL
                    self.bus.send_frame(
                        FRAME_TYPE_CAL_ACK,
                        self.device_id,
                        bytes([cmd, status, self.active_sensors & 0xFF]),
                        seq,
                    )
                    self.counters["resp"] += 1
                    if status == CAL_ACK_OK:
                        time.sleep(0.1)
                        reset_board()
                elif cmd == CAL_CMD_RESET:
                    flags_ok = True
                    flags_ok &= nvm_flags.set_usb_drive_disabled(True)
                    flags_ok &= nvm_flags.set_reset_calibration_on_boot(True)
                    status = CAL_ACK_OK if flags_ok else CAL_ACK_FAIL
                    self.bus.send_frame(
                        FRAME_TYPE_CAL_ACK,
                        self.device_id,
                        bytes([cmd, status, self.active_sensors & 0xFF]),
                        seq,
                    )
                    self.counters["resp"] += 1
                    if status == CAL_ACK_OK:
                        print("RS485 reset command received; entering calibration mode.")
                        time.sleep(0.1)
                        reset_board()
            elif frame_type == FRAME_TYPE_MINMAX_REQ:
                if len(payload) >= 2:
                    start = payload[0]
                    count = payload[1]
                else:
                    start = 0
                    count = self.active_sensors
                max_count = max((self.bus_max_payload - 2) // MINMAX_RECORD_BYTES, 0)
                if start >= self.active_sensors:
                    count = 0
                else:
                    count = min(count, max_count, self.active_sensors - start)
                resp = bytearray(2 + (count * MINMAX_RECORD_BYTES))
                resp[0] = start
                resp[1] = count
                for idx in range(count):
                    sensor_idx = start + idx
                    offset = 2 + (idx * MINMAX_RECORD_BYTES)
                    resp[offset : offset + 2] = int(self.state.sensor_min[sensor_idx]).to_bytes(2, "little")
                    resp[offset + 2 : offset + 4] = int(self.state.sensor_max[sensor_idx]).to_bytes(2, "little")
                    resp[offset + 4 : offset + 6] = int(
                        self.stats.std_for_sensor(sensor_idx, self.state.sensor_disabled)
                    ).to_bytes(2, "little")
                self.bus.send_frame(FRAME_TYPE_MINMAX_RESP, self.device_id, resp, seq)
                self.counters["resp"] += 1
            elif frame_type == FRAME_TYPE_TRACE_REQ:
                if self.trace is None or not self.trace.enabled:
                    continue
                if len(payload) < 2:
                    continue
                sensor_idx = payload[0]
                chunk_idx = payload[1]
                if sensor_idx >= self.active_sensors or chunk_idx >= self.trace.trace_chunk_count:
                    continue
                result = self.trace.build_trace_response(sensor_idx, chunk_idx)
                if result is None:
                    continue
                resp, valid_samples = result
                resp[2:4] = int(valid_samples).to_bytes(2, "little")
                self.bus.send_frame(FRAME_TYPE_TRACE_RESP, self.device_id, resp, seq)
                self.counters["resp"] += 1
            elif frame_type == FRAME_TYPE_EVENT_ACK:
                self.event_engine.ack(seq)
