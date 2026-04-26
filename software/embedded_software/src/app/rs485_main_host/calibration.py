"""Calibration workflows for the RS-485 main host.

Requests calibration save/reset from sensor nodes over the bus, assembles
min/max payloads, writes calibration to flash, and manages NVM flags + reboots.
"""

from __future__ import annotations

import time

import microcontroller
import storage
import supervisor

from app import nvm_flags
from app import sensor_calibration
from photon_rs485 import (
    FRAME_TYPE_EVENT,
    FRAME_TYPE_EVENT_ACK,
    FRAME_TYPE_MINMAX_RESP,
)

from .constants import (
    CAL_ACK_FAIL,
    CAL_ACK_OK,
    CAL_CMD_RESET,
    CAL_CMD_SAVE,
    CAL_CMD_TIMEOUT_S,
    CALIBRATION_PATH,
    FRAME_TYPE_CAL_ACK,
    FRAME_TYPE_CAL_CMD,
    MAX_SENSORS,
    SENSOR_NODE_DEVICE_IDS,
    SENSOR_VALUE_MAX,
)
from .protocol import apply_minmax_payload_local


def _reset_board() -> bool:
    try:
        microcontroller.reset()
        return True
    except Exception:
        pass
    try:
        supervisor.reload()
        return True
    except Exception:
        pass
    return False


def _get_root_readonly() -> bool | None:
    try:
        mount = storage.getmount("/")
    except Exception:
        return None
    return bool(getattr(mount, "readonly", False))


def _remount_root(readonly: bool) -> bool:
    try:
        storage.remount("/", readonly=readonly)
    except Exception:
        return False
    return True


def send_cal_cmd(bus, target_id: int, cmd: int, seq: int) -> None:
    bus.send_frame(FRAME_TYPE_CAL_CMD, target_id, bytes([cmd & 0xFF]), seq & 0xFFFF, ack_timeout_us=0)


def collect_calibration_from_sensor_node(bus, sensor_device_id: int, seq: int, timeout_s: float):
    mins = [SENSOR_VALUE_MAX] * MAX_SENSORS
    maxs = [0] * MAX_SENSORS
    seen = [False] * MAX_SENSORS
    seen_count = 0
    ack_status = None
    expected_count = MAX_SENSORS
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        for frame_type, target_id, source_id, payload, rx_seq in bus.read_frames():
            if frame_type == FRAME_TYPE_EVENT:
                event_seq = rx_seq
                if len(payload) >= 7:
                    event_seq = int.from_bytes(payload[4:6], "little")
                elif len(payload) >= 5:
                    event_seq = int.from_bytes(payload[3:5], "little")
                bus.send_frame(FRAME_TYPE_EVENT_ACK, source_id, b"", event_seq, ack_timeout_us=0)
                continue
            if source_id != sensor_device_id:
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
        for frame_type, target_id, source_id, payload, rx_seq in bus.read_frames():
            if frame_type == FRAME_TYPE_EVENT:
                event_seq = rx_seq
                if len(payload) >= 7:
                    event_seq = int.from_bytes(payload[4:6], "little")
                elif len(payload) >= 5:
                    event_seq = int.from_bytes(payload[3:5], "little")
                bus.send_frame(FRAME_TYPE_EVENT_ACK, source_id, b"", event_seq, ack_timeout_us=0)
                continue
            if (
                frame_type == FRAME_TYPE_CAL_ACK
                and source_id == sensor_device_id
                and rx_seq == (seq & 0xFFFF)
                and len(payload) >= 2
                and payload[0] == cmd
            ):
                return payload[1] == CAL_ACK_OK
    return False


def save_calibration_payload(payload: dict) -> bool:
    readonly = _get_root_readonly()
    restore_readonly = False
    if readonly is True:
        if not _remount_root(False):
            return False
        restore_readonly = True
    ok = sensor_calibration.save_calibration_file(CALIBRATION_PATH, payload)
    if restore_readonly:
        _remount_root(True)
    return ok


def handle_cal_save(bus, cal_seq_ref: list[int], *, max_retries: int = 3) -> bool:
    print("Requesting calibration save from sensor nodes...")
    payload = sensor_calibration.load_calibration_file(CALIBRATION_PATH)
    collected = {}
    failed_nodes = list(SENSOR_NODE_DEVICE_IDS)
    attempt = 0
    while failed_nodes and attempt < max_retries:
        attempt += 1
        if attempt > 1:
            print(f"Retry {attempt - 1}/{max_retries - 1} for node(s) {failed_nodes}...")
        still_failed = []
        for sensor_device_id in failed_nodes:
            seq = cal_seq_ref[0] & 0xFFFF
            cal_seq_ref[0] = (cal_seq_ref[0] + 1) & 0xFFFF
            send_cal_cmd(bus, sensor_device_id, CAL_CMD_SAVE, seq)
            mins, maxs, seen, ack_ok, expected_count = collect_calibration_from_sensor_node(
                bus, sensor_device_id, seq, CAL_CMD_TIMEOUT_S
            )
            seen_count = sum(1 for entry in seen if entry)
            if not ack_ok:
                print(f"Calibration save ack failed for sensor node {sensor_device_id}.")
                still_failed.append(sensor_device_id)
            elif seen_count < expected_count:
                missing = expected_count - seen_count
                print(f"Calibration data missing for sensor node {sensor_device_id}: {missing} sensor(s).")
                still_failed.append(sensor_device_id)
            else:
                collected[sensor_device_id] = (mins, maxs, expected_count)
        failed_nodes = still_failed
    if failed_nodes:
        print(f"Calibration save aborted; no ack/data from node(s) {failed_nodes} after {max_retries} attempts.")
        return False
    for sensor_device_id in SENSOR_NODE_DEVICE_IDS:
        mins, maxs, expected_count = collected[sensor_device_id]
        payload = sensor_calibration.build_payload(
            payload,
            sensor_device_id,
            expected_count,
            [int(v) for v in mins[:expected_count]],
            [int(v) for v in maxs[:expected_count]],
        )
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


def handle_cal_reset(bus, cal_seq_ref: list[int], *, max_retries: int = 3) -> bool:
    print("Requesting calibration reset on all sensor nodes...")
    failed_nodes = list(SENSOR_NODE_DEVICE_IDS)
    attempt = 0
    while failed_nodes and attempt < max_retries:
        attempt += 1
        if attempt > 1:
            print(f"Retry {attempt - 1}/{max_retries - 1} for node(s) {failed_nodes}...")
        still_failed = []
        for sensor_device_id in failed_nodes:
            seq = cal_seq_ref[0] & 0xFFFF
            cal_seq_ref[0] = (cal_seq_ref[0] + 1) & 0xFFFF
            send_cal_cmd(bus, sensor_device_id, CAL_CMD_RESET, seq)
            if not wait_for_cal_ack(bus, sensor_device_id, CAL_CMD_RESET, seq, CAL_CMD_TIMEOUT_S):
                print(f"Calibration reset ack failed for sensor node {sensor_device_id}.")
                still_failed.append(sensor_device_id)
        failed_nodes = still_failed
    if failed_nodes:
        print(f"Calibration reset aborted; no ack from node(s) {failed_nodes} after {max_retries} attempts.")
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
    if not _reset_board():
        return
