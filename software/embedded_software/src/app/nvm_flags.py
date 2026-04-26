"""Persist small boot/runtime flags in microcontroller NVM.

Byte 0 layout (bit = 1 means enabled):
- bit 0: disable USB MSC (make flash writable)
- bit 1: reset calibration on boot (skip loading default cal)
"""

from __future__ import annotations

import microcontroller

NVM_INDEX = 0
FLAG_USB_DRIVE_DISABLED = 0x01
FLAG_RESET_CAL_ON_BOOT = 0x02

# CircuitPython can only disable MSC at boot; writes need MSC disabled first.


def _read_byte() -> int | None:
    nvm = microcontroller.nvm
    if nvm is None or len(nvm) <= NVM_INDEX:
        return None
    return int(nvm[NVM_INDEX])


def _write_byte(value: int) -> bool:
    nvm = microcontroller.nvm
    if nvm is None or len(nvm) <= NVM_INDEX:
        return False
    value &= 0xFF
    try:
        if int(nvm[NVM_INDEX]) == value:
            return True
        nvm[NVM_INDEX] = value
    except Exception:
        return False
    return True


def get_flags(default: int = 0) -> int:
    value = _read_byte()
    if value is None:
        return default
    return value


def set_flags(value: int) -> bool:
    return _write_byte(value)


def is_usb_drive_disabled() -> bool:
    return bool(get_flags() & FLAG_USB_DRIVE_DISABLED)


def set_usb_drive_disabled(enabled: bool) -> bool:
    value = get_flags()
    if enabled:
        value |= FLAG_USB_DRIVE_DISABLED
    else:
        value &= ~FLAG_USB_DRIVE_DISABLED
    return set_flags(value)


def reset_calibration_on_boot() -> bool:
    return bool(get_flags() & FLAG_RESET_CAL_ON_BOOT)


def set_reset_calibration_on_boot(enabled: bool) -> bool:
    value = get_flags()
    if enabled:
        value |= FLAG_RESET_CAL_ON_BOOT
    else:
        value &= ~FLAG_RESET_CAL_ON_BOOT
    return set_flags(value)


def describe_flags(value: int | None = None) -> str:
    if value is None:
        value = _read_byte()
    if value is None:
        return "nvm unavailable"
    parts = ["0x%02X" % value]
    parts.append("usb_msc=off" if value & FLAG_USB_DRIVE_DISABLED else "usb_msc=on")
    parts.append("reset_cal=on" if value & FLAG_RESET_CAL_ON_BOOT else "reset_cal=off")
    return " ".join(parts)
