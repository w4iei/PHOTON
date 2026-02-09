"""Calibration workflows for the RS-485 main host.

Requests calibration save/reset from sensor nodes over the bus, assembles
min/max payloads, writes calibration to flash, and manages NVM flags + reboots.
"""

from __future__ import annotations

import time

from app.helpers import nvm_flags
from app.helpers import sensor_calibration
from app.rs485_bus import (
    FRAME_TYPE_CAL_ACK,
    FRAME_TYPE_CAL_CMD,
    FRAME_TYPE_EVENT,
    FRAME_TYPE_EVENT_ACK,
    FRAME_TYPE_MINMAX_RESP,
)
from app.rs485_common.reset import reset_board
from app.rs485_common.storage import get_root_readonly, remount_root

from .constants import (
    CAL_ACK_FAIL,
    CAL_ACK_OK,
    CAL_CMD_RESET,
    CAL_CMD_SAVE,
    CAL_CMD_TIMEOUT_S,
    CALIBRATION_PATH,
    MAX_SENSORS,
    SENSOR_NODE_DEVICE_IDS,
    SENSOR_VALUE_MAX,
)
from .protocol import apply_minmax_payload_local


def send_cal_cmd(bus, target_id: int, cmd: int, seq: int) -> None:
    bus.send_frame(FRAME_TYPE_CAL_CMD, target_id, bytes([cmd & 0xFF]), seq & 0xFFFF)


def collect_calibration_from_sensor_node(bus, sensor_device_id: int, seq: int, timeout_s: float):
    mins = [SENSOR_VALUE_MAX] * MAX_SENSORS
    maxs = [0] * MAX_SENSORS
    seen = [False] * MAX_SENSORS
    seen_count = 0
    ack_status = None
    expected_count = MAX_SENSORS
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        for frame_type, target_id, payload, rx_seq in bus.read_frames():
            if frame_type == FRAME_TYPE_EVENT:
                event_seq = rx_seq
                if len(payload) >= 7:
                    event_seq = int.from_bytes(payload[4:6], "little")
                elif len(payload) >= 5:
                    event_seq = int.from_bytes(payload[3:5], "little")
                bus.send_frame(FRAME_TYPE_EVENT_ACK, target_id, b"", event_seq)
                continue
            if target_id != sensor_device_id:
                continue
            if frame_type == FRAME_TYPE_MINMAX_RESP:
                seen_count += apply_minmax_payload_local(payload, mins, maxs, seen)
            elif frame_type == FRAME_TYPE_CAL_ACK and rx_seq == (seq & 0xFFFF):
                if len(payload) >= 2 and payload[0] == CAL_CMD_SAVE:
                    ack_status = payload[1] == CAL_ACK_OK
                else:
                    ack_status = False
                if len(payload) >= 3:
                    expected_count = min(int(payload[2]), MAX_SENSORS)
        if ack_status is not None and seen_count >= expected_count:
            break
    return mins, maxs, seen, ack_status, expected_count


def wait_for_cal_ack(bus, sensor_device_id: int, cmd: int, seq: int, timeout_s: float) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        for frame_type, target_id, payload, rx_seq in bus.read_frames():
            if frame_type == FRAME_TYPE_EVENT:
                event_seq = rx_seq
                if len(payload) >= 7:
                    event_seq = int.from_bytes(payload[4:6], "little")
                elif len(payload) >= 5:
                    event_seq = int.from_bytes(payload[3:5], "little")
                bus.send_frame(FRAME_TYPE_EVENT_ACK, target_id, b"", event_seq)
                continue
            if (
                frame_type == FRAME_TYPE_CAL_ACK
                and target_id == sensor_device_id
                and rx_seq == (seq & 0xFFFF)
                and len(payload) >= 2
                and payload[0] == cmd
            ):
                return payload[1] == CAL_ACK_OK
    return False


def save_calibration_payload(payload: dict) -> bool:
    readonly = get_root_readonly()
    restore_readonly = False
    if readonly is True:
        if not remount_root(False):
            return False
        restore_readonly = True
    ok = sensor_calibration.save_calibration_file(CALIBRATION_PATH, payload)
    if restore_readonly:
        remount_root(True)
    return ok


def handle_cal_save(bus, cal_seq_ref: list[int]) -> bool:
    print("Requesting calibration save from sensor nodes...")
    payload = sensor_calibration.load_calibration_file(CALIBRATION_PATH)
    all_ok = True
    for sensor_device_id in SENSOR_NODE_DEVICE_IDS:
        seq = cal_seq_ref[0] & 0xFFFF
        cal_seq_ref[0] = (cal_seq_ref[0] + 1) & 0xFFFF
        send_cal_cmd(bus, sensor_device_id, CAL_CMD_SAVE, seq)
        mins, maxs, seen, ack_ok, expected_count = collect_calibration_from_sensor_node(
            bus, sensor_device_id, seq, CAL_CMD_TIMEOUT_S
        )
        if not ack_ok:
            print(f"Calibration save ack failed for sensor node {sensor_device_id}.")
            all_ok = False
        seen_count = sum(1 for entry in seen if entry)
        if seen_count < expected_count:
            missing = expected_count - seen_count
            print(f"Calibration data missing for sensor node {sensor_device_id}: {missing} sensor(s).")
            all_ok = False
        if ack_ok and seen_count >= expected_count:
            payload = sensor_calibration.build_payload(
                payload,
                sensor_device_id,
                expected_count,
                [int(v) for v in mins[:expected_count]],
                [int(v) for v in maxs[:expected_count]],
            )
    if not all_ok:
        print("Calibration save aborted; missing ack/data.")
        return False
    if payload is None:
        print("Calibration save aborted; no payload to write.")
        return False
    if not save_calibration_payload(payload):
        print("Calibration save failed; file not written.")
        return False
    flags_ok = nvm_flags.set_usb_drive_disabled(False) and nvm_flags.set_reset_calibration_on_boot(False)
    if not flags_ok:
        print("NVM flag update failed; rebooting anyway.")
    print("Calibration saved from all sensor nodes. Rebooting...")
    reset_reboot("Main host rebooting after calibration save.")
    return True


def handle_cal_reset(bus, cal_seq_ref: list[int]) -> bool:
    print("Requesting calibration reset on all sensor nodes...")
    all_ok = True
    for sensor_device_id in SENSOR_NODE_DEVICE_IDS:
        seq = cal_seq_ref[0] & 0xFFFF
        cal_seq_ref[0] = (cal_seq_ref[0] + 1) & 0xFFFF
        send_cal_cmd(bus, sensor_device_id, CAL_CMD_RESET, seq)
        if not wait_for_cal_ack(bus, sensor_device_id, CAL_CMD_RESET, seq, CAL_CMD_TIMEOUT_S):
            print(f"Calibration reset ack failed for sensor node {sensor_device_id}.")
            all_ok = False
    if not all_ok:
        print("Calibration reset aborted; missing ack(s).")
        return False
    flags_ok = nvm_flags.set_usb_drive_disabled(True) and nvm_flags.set_reset_calibration_on_boot(True)
    if not flags_ok:
        print("NVM flag update failed; rebooting anyway.")
    print("Calibration reset confirmed. Rebooting...")
    reset_reboot("Main host rebooting into calibration mode.")
    return True


def reset_reboot(reason: str) -> None:
    print(reason)
    time.sleep(0.1)
    if not reset_board():
        return
