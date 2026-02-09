"""Calibration workflows for the RS-485 sensor node.

Loads/applies calibration payloads, captures baseline values, saves calibration to
flash, and manages NVM flags + reboots when entering or exiting calibration mode.
"""

from __future__ import annotations

import time

from app.helpers import nvm_flags
from app.helpers import sensor_calibration

from .constants import SENSOR_VALUE_MAX
from app.rs485_common.reset import reset_board
from app.rs485_common.storage import get_root_readonly, remount_root
from .runtime import supervisor
from .stats import refresh_data_payload


def print_disabled_sensor_warning(disabled_count: int) -> None:
    if disabled_count <= 0:
        return
    lines = [
        f"WARNING: {disabled_count} sensor(s) disabled.",
        "Disabled due to high level at boot OR config override.",
        "Please be sure that no keys are pressed",
        "when powering on the photon system.",
        "Config source: /config/rs485_sensor_node.json -> disabled_sensors",
    ]
    frame_width = max(len(line) for line in lines)
    print("+-" + ("-" * frame_width) + "-+")
    for line in lines:
        padding = frame_width - len(line)
        if padding < 0:
            padding = 0
        print("| " + line + (" " * padding) + " |")
    print("+-" + ("-" * frame_width) + "-+")


def print_cal_save_unavailable_warning() -> None:
    lines = [
        "ERROR: Calibration save is unavailable.",
        "NVM flags indicate usb_msc=on with reset_cal=on.",
        "Saving calibration requires MSC disabled.",
        "Fix: use [R] to reboot into calibration mode",
        "or set usb_msc=off in NVM flags.",
    ]
    frame_width = max(len(line) for line in lines)
    print("+-" + ("-" * frame_width) + "-+")
    for line in lines:
        padding = frame_width - len(line)
        if padding < 0:
            padding = 0
        print("| " + line + (" " * padding) + " |")
    print("+-" + ("-" * frame_width) + "-+")


class CalibrationManager:
    def __init__(
        self,
        *,
        device_id: int,
        active_sensors: int,
        boot_disable_above: int,
        disabled_sensors_cfg: set[int],
        scanner,
        state,
        stats,
        calibration_path: str,
    ):
        self.device_id = device_id
        self.active_sensors = active_sensors
        self.boot_disable_above = boot_disable_above
        self.disabled_sensors_cfg = disabled_sensors_cfg
        self.scanner = scanner
        self.state = state
        self.stats = stats
        self.calibration_path = calibration_path

    def init_baseline(self) -> None:
        self.scanner.scan_into(self.state.scan_buffer)
        disabled = []
        for sensor_idx in range(self.active_sensors):
            value = int(self.state.scan_buffer[sensor_idx])
            self.state.sensor_baseline[sensor_idx] = value
            self.state.latest_values[sensor_idx] = value
            self.state.sensor_min[sensor_idx] = value
            self.state.sensor_max[sensor_idx] = value
            if value > self.boot_disable_above:
                self.state.sensor_disabled[sensor_idx] = True
                disabled.append(sensor_idx)
            if sensor_idx in self.disabled_sensors_cfg:
                self.state.sensor_disabled[sensor_idx] = True
                self.state.sensor_baseline[sensor_idx] = 0
                self.state.latest_values[sensor_idx] = 0
                self.state.sensor_min[sensor_idx] = 0
                self.state.sensor_max[sensor_idx] = 0
        if disabled:
            print(
                f"Disabled sensors at boot (value > {self.boot_disable_above}): "
                f"{', '.join(str(idx) for idx in disabled)}"
            )
        if self.disabled_sensors_cfg:
            print(
                "Disabled sensors by config: "
                f"{', '.join(str(idx) for idx in sorted(self.disabled_sensors_cfg))}"
            )
        disabled_total = sum(1 for is_disabled in self.state.sensor_disabled if is_disabled)
        print_disabled_sensor_warning(disabled_total)

    def apply_calibration_payload(self, payload: dict) -> bool:
        if not isinstance(payload, dict):
            return False
        mins = payload.get("min")
        maxs = payload.get("max")
        count = payload.get("active_sensors", self.active_sensors)
        if not isinstance(mins, list) or not isinstance(maxs, list):
            return False
        if count != self.active_sensors or len(mins) != self.active_sensors or len(maxs) != self.active_sensors:
            return False
        for sensor_idx in range(self.active_sensors):
            try:
                min_v = int(mins[sensor_idx])
                max_v = int(maxs[sensor_idx])
            except Exception:
                continue
            if min_v < 0:
                min_v = 0
            if max_v < 0:
                max_v = 0
            if min_v > SENSOR_VALUE_MAX:
                min_v = SENSOR_VALUE_MAX
            if max_v > SENSOR_VALUE_MAX:
                max_v = SENSOR_VALUE_MAX
            if max_v < min_v:
                min_v, max_v = max_v, min_v
            self.state.sensor_min[sensor_idx] = min_v
            self.state.sensor_max[sensor_idx] = max_v
        return True

    def save_calibration_payload(self) -> bool:
        existing = sensor_calibration.load_calibration_file(self.calibration_path)
        payload = sensor_calibration.build_payload(
            existing,
            self.device_id,
            self.active_sensors,
            [int(value) for value in self.state.sensor_min],
            [int(value) for value in self.state.sensor_max],
        )
        debug = []

        def _print_save_debug() -> None:
            print("Save calibration failed.")
            for line in debug:
                print("  - " + line)

        if supervisor is not None:
            try:
                debug.append("usb_connected=%s" % supervisor.runtime.usb_connected)
            except Exception as exc:
                debug.append(
                    "usb_connected check failed: %s: %s" % (exc.__class__.__name__, exc)
                )

        restore_readonly = False
        readonly = get_root_readonly()
        debug.append("nvm_usb_disable=%s" % nvm_flags.is_usb_drive_disabled())
        debug.append("root_readonly=%s" % readonly)
        if readonly is not False:
            if not remount_root(False, debug=debug):
                debug.append("hint: set NVM flag via app.helpers.nvm_flags.set_usb_drive_disabled(True)")
                debug.append("save_aborted_before_write")
                _print_save_debug()
                return False
            restore_readonly = True
        ok = sensor_calibration.save_calibration_file(self.calibration_path, payload, debug=debug)
        if restore_readonly:
            if not remount_root(True, debug=debug):
                debug.append("failed_to_restore_readonly")
        if ok:
            print("Saved calibration to", self.calibration_path)
        else:
            _print_save_debug()
        return ok

    def load_calibration_from_flash(self) -> tuple[bool, str | None, bool]:
        payload = sensor_calibration.load_calibration_file(self.calibration_path)
        if payload is None:
            return False, "Calibration file missing at %s" % self.calibration_path, True
        node_payload = sensor_calibration.extract_node_calibration(payload, self.device_id)
        if node_payload is None:
            msg = "Calibration for device %d not found in %s" % (self.device_id, self.calibration_path)
            return False, msg, True
        if not self.apply_calibration_payload(node_payload):
            msg = "Calibration for device %d invalid in %s" % (self.device_id, self.calibration_path)
            return False, msg, True
        msg = "Loaded calibration from %s (device %d)" % (self.calibration_path, self.device_id)
        return True, msg, False

    def enter_calibration_mode(self, reason: str) -> None:
        print(reason)
        nvm_flags.set_usb_drive_disabled(True)
        nvm_flags.set_reset_calibration_on_boot(True)
        print("Entering calibration mode (NVM flags set). Rebooting...")
        time.sleep(0.1)
        if reset_board():
            return
        print("Reset failed; please power-cycle the board.")

    def reset_calibration(self) -> None:
        self.init_baseline()
        self.stats.update(self.state.scan_buffer, self.state.sensor_disabled)
        refresh_data_payload(
            self.state.data_payload,
            self.state.scan_buffer,
            self.state.sensor_disabled,
            self.stats,
        )
