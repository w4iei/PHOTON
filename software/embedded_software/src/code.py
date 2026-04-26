"""
Board entry point with manual run-mode list.

Available run modes (edit this docstring when adding new ones):
- heartbeat
- midi-file
- midi-file-stream
- screen-test
- rt-sensor-display
- microsd-midi-write
- rs485-sensor-node
- rs485-main-host
- sensor-node-single-sensor-trace
"""

from __future__ import annotations

import os
import supervisor
import time

from app import nvm_flags

# Default fallback for local bench work when no role marker files are present.
ACTIVE_MODE = "screen-test"


def run_mode(name: str) -> None:
    """
    Import and execute the requested mode. Only imports the active mode to
    avoid claiming pins from other modules.
    """

    if name == "heartbeat":
        from app import heartbeat_midi

        heartbeat_midi.main()
    elif name == "midi-file":
        from app import play_midi_file

        play_midi_file.main()
    elif name == "midi-file-stream":
        from app import play_midi_file

        play_midi_file.main2(filename="/assets/bwv832.mid")
    elif name == "screen-test":
        from app import screen_test

        screen_test.main()
    elif name == "rt-sensor-display":
        from app import rt_sensor_display

        rt_sensor_display.main()
    elif name == "microsd-midi-write":
        from app import microsd_midi_write

        microsd_midi_write.main()
    elif name == "rs485-sensor-node":
        from app import rs485_sensor_node

        rs485_sensor_node.main()
    elif name == "rs485-main-host":
        from app import rs485_main_host

        rs485_main_host.main()
    elif name == "sensor-node-single-sensor-trace":
        from app import sensor_node_single_sensor_trace

        sensor_node_single_sensor_trace.main()
    else:
        raise ValueError("Unknown run mode '%s'" % name)


def wait_for_serial(timeout: float = 3.0) -> None:
    """Pause at reset so the host has time to open the USB serial port."""
    t0 = time.monotonic()
    while (not supervisor.runtime.serial_connected) and (time.monotonic() - t0 < timeout):
        pass
    # Tiny delay so the host finishes opening the port
    time.sleep(0.25)


def _file_exists(path: str) -> bool:
    try:
        os.stat(path)
    except OSError:
        return False
    return True


def resolve_active_mode() -> str:
    if _file_exists("/main"):
        return "rs485-main-host"
    if _file_exists("/sensor_node_id") or _file_exists("/secondary_sensor"):
        return "rs485-sensor-node"
    return ACTIVE_MODE


def main() -> None:
    wait_for_serial(timeout=1.0)
    import gc

    ram_kb = gc.mem_free() // 1024
    print("[BOOT] PSRAM Present" if ram_kb > 1024 else "[BOOT] RAM free: %d KB" % ram_kb)
    print("[BOOT] NVM flags:", nvm_flags.describe_flags())
    try:
        run_mode(resolve_active_mode())
    except ValueError as exc:
        if not str(exc).startswith("Unknown run mode"):
            raise
        print(exc)
        print("Update ACTIVE_MODE to one of the modes listed in the docstring.")
        raise


if __name__ == "__main__":
    main()
