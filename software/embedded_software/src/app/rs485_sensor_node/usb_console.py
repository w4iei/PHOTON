"""USB console handling for debugging the RS-485 sensor node.

Used from the main loop when USB serial is connected and `usb_verbose` is enabled.
Keyboard inputs: Enter prints the sensor table; `s` saves calibration + reboots;
`r` enters calibration mode; `x` exits without saving + reboots (case-insensitive).
"""

from __future__ import annotations

import time

from app.helpers import nvm_flags
from app.rs485_common.serial import read_serial_input
from app.rs485_common.reset import reset_board


class USBConsole:
    def __init__(self, *, active_sensors: int, state, stats, calibration):
        self.active_sensors = active_sensors
        self.state = state
        self.stats = stats
        self.calibration = calibration

    def print_table(self) -> None:
        header = ["idx", "val", "min", "max", "rng", "std", "on", "act"]
        print("\n# Sensor table")
        print(
            "{:>3s} | {:>5s} | {:>5s} | {:>5s} | {:>5s} | {:>5s} | {:>2s} | {:>3s}".format(
                *header
            )
        )
        print("-" * 67)
        for sensor_idx in range(self.active_sensors):
            min_v = self.state.sensor_min[sensor_idx]
            max_v = self.state.sensor_max[sensor_idx]
            rng = max(0, int(max_v) - int(min_v))
            std_v = self.stats.std_for_sensor(sensor_idx, self.state.sensor_disabled)
            row = [
                str(sensor_idx),
                str(int(self.state.latest_values[sensor_idx])),
                str(int(min_v)),
                str(int(max_v)),
                str(int(rng)),
                str(int(std_v)),
                "1" if self.state.sensor_on[sensor_idx] else "0",
                "1" if self.state.sensor_active[sensor_idx] else "0",
            ]
            print(
                "{:>3s} | {:>5s} | {:>5s} | {:>5s} | {:>5s} | {:>5s} | {:>2s} | {:>3s}".format(
                    *row
                )
            )

    def handle_input(self) -> None:
        data = read_serial_input()
        if not data:
            return
        for ch in data:
            if ch in ("\n", "\r"):
                self.print_table()
                continue
            if ch in ("s", "S"):
                self.calibration.save_calibration_payload()
                nvm_flags.set_usb_drive_disabled(False)
                nvm_flags.set_reset_calibration_on_boot(False)
                print("NVM flags set: MSC enabled, reset-cal disabled. Rebooting...")
                time.sleep(0.1)
                if not reset_board():
                    print("Reset failed; please power-cycle the board.")
            elif ch in ("r", "R"):
                self.calibration.enter_calibration_mode(
                    "USB reset command received; entering calibration mode."
                )
            elif ch in ("x", "X"):
                nvm_flags.set_usb_drive_disabled(False)
                nvm_flags.set_reset_calibration_on_boot(False)
                print("Exiting calibration without saving. Rebooting...")
                time.sleep(0.1)
                if not reset_board():
                    print("Reset failed; please power-cycle the board.")
