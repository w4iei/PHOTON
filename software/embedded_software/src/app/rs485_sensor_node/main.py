"""RS-485 sensor node (run mode 'rs485-sensor-node')."""

from __future__ import annotations

import time

from app.helpers import nvm_flags
from app.rs485_bus import (
    FRAME_TYPE_DATA_REQ,
    FRAME_TYPE_DATA_RESP,
    FRAME_TYPE_PING,
    FRAME_TYPE_PONG,
)

from .calibration import CalibrationManager, print_cal_save_unavailable_warning
from .config import deep_merge, load_config, parse_disabled_sensors
from .constants import (
    CALIBRATION_PATH,
    CONFIG_PATH,
    DEFAULT_CONFIG,
    LOG_INTERVAL_S,
    STATS_WINDOW_S,
)
from .events import EventEngine
from .hardware_setup import setup_outputs, setup_rs485, setup_scanner
from .state import NodeState
from .stats import ScanRateTracker, StatsTracker, refresh_data_payload
from .trace import TraceRecorder
from app.rs485_common.serial import serial_poll_requested, usb_connected
from .usb_console import USBConsole
from .protocol import FrameDispatcher


def _clamp_pct(value: int, default: int) -> int:
    try:
        value = int(value)
    except Exception:
        return default
    return max(0, min(100, value))


def main() -> None:
    cfg = deep_merge(DEFAULT_CONFIG, load_config(CONFIG_PATH))
    device_id = int(cfg["device_id"])
    active_sensors = int(cfg["active_sensors"])
    settle_us = int(cfg["settle_us"])
    event_enabled = bool(cfg.get("event_mode", False))
    event_ack_timeout_s = float(cfg.get("event_ack_timeout_s", 0.005))
    event_retry_max = int(cfg.get("event_retry_max", 4))
    event_backoff_us = cfg.get("event_backoff_us", [50, 200])
    event_queue_max = int(cfg.get("event_queue_max", 64))
    min_event_range = int(cfg.get("min_event_range", 170))
    strike_pct_default = int(cfg.get("strike_pct", 60))
    release_pct = int(cfg.get("release_pct", 40))
    activation_pct = int(cfg.get("activation_pct", 3))
    boot_disable_above = int(cfg.get("boot_disable_above", 3_000))
    trace_slots = int(cfg.get("trace_slots", 10))
    trace_seconds = int(cfg.get("trace_seconds", 10))
    trace_sample_hz = int(cfg.get("trace_sample_hz", 100))
    trace_hold_s = float(cfg.get("trace_hold_s", 1.2))
    usb_verbose = bool(cfg.get("usb_verbose", True))
    log_event_details = True
    disabled_sensors_cfg = parse_disabled_sensors(cfg.get("disabled_sensors"), active_sensors)
    reset_cal_on_boot = nvm_flags.reset_calibration_on_boot()

    strike_pct_default = _clamp_pct(strike_pct_default, 60)
    release_pct = _clamp_pct(release_pct, 40)
    activation_pct = _clamp_pct(activation_pct, 3)
    if boot_disable_above < 0:
        boot_disable_above = 0

    setup_outputs(cfg)
    scanner = setup_scanner(cfg, active_sensors, settle_us)
    bus, use_fast = setup_rs485(cfg, device_id, active_sensors * 6)

    if usb_verbose:
        time.sleep(0.5)
    print("")
    print(".*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*")
    print(".*.*.*.*  PHOTON RS485 Sensor Node  .*.*.*")
    print(".*.   https://github.com/w4iei/photon   .*")
    print(".*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*")
    print("Creator: Noah Jaffe")
    print("")
    print("--- RS485 Sensor Node ---")
    print(f"Device ID: {device_id}")
    print(f"Active sensors: {active_sensors}")
    print("Scan rate: unthrottled")
    print(f"Settle time before sensor ADC capture: {settle_us} us")
    print(f"RS485 term (inside RS485 driver): {bool(cfg['left_term'])}")
    rs485_driver_name = "photon_rs485" if use_fast else "python"
    rs485_driver_kind = "C driver" if use_fast else "Python driver"
    print(f"RS485 driver: {rs485_driver_name} ({rs485_driver_kind})")
    scan_driver_name = "photon_sensorscan" if scanner.use_c else "python"
    scan_driver_kind = "C driver" if scanner.use_c else "Python driver"
    print(f"Sensor scan: {scan_driver_name} ({scan_driver_kind})")
    print(f"Event mode: {event_enabled}")
    print("NVM flags:", nvm_flags.describe_flags())
    if reset_cal_on_boot and not nvm_flags.is_usb_drive_disabled():
        print_cal_save_unavailable_warning()
    if not scanner.use_c:
        print("!!! WARNING: Python sensor scan is active; performance will be slower. !!!")
        print("!!! WARNING: Install the photon CircuitPython build to enable C scanning. !!!")
    if usb_verbose:
        command_lines = ["Press Enter to print the sensor table over USB serial."]
        command_lines.append("[R] Reboot into calibration mode (disable MSC).")
        if reset_cal_on_boot:
            command_lines.insert(0, "[S] Save calibration + re-enable MSC on next boot.")
            command_lines.insert(1, "[X] Exit calibration without saving + reboot.")
        frame_width = max(len(line) for line in command_lines)
        print("+-" + ("-" * frame_width) + "-+")
        for line in command_lines:
            padding = frame_width - len(line)
            if padding < 0:
                padding = 0
            print("| " + line + (" " * padding) + " |")
        print("+-" + ("-" * frame_width) + "-+")

    state = NodeState(active_sensors, strike_pct_default)
    stats = StatsTracker(active_sensors)
    calibration = CalibrationManager(
        device_id=device_id,
        active_sensors=active_sensors,
        boot_disable_above=boot_disable_above,
        disabled_sensors_cfg=disabled_sensors_cfg,
        scanner=scanner,
        state=state,
        stats=stats,
        calibration_path=CALIBRATION_PATH,
    )

    calibration.init_baseline()
    cal_status_msg = None
    if reset_cal_on_boot:
        cal_status_msg = "Calibration not loaded (reset mode). File: %s" % CALIBRATION_PATH
    else:
        _, cal_status_msg, needs_reset = calibration.load_calibration_from_flash()
        if needs_reset and cal_status_msg:
            calibration.enter_calibration_mode(cal_status_msg)
            return
    stats.update(state.scan_buffer, state.sensor_disabled)
    refresh_data_payload(state.data_payload, state.scan_buffer, state.sensor_disabled, stats)

    auto_reply_ping = False
    auto_reply_data = False
    if use_fast and hasattr(bus, "add_auto_reply"):
        bus.add_auto_reply(FRAME_TYPE_PING, FRAME_TYPE_PONG, b"")
        bus.add_auto_reply(FRAME_TYPE_DATA_REQ, FRAME_TYPE_DATA_RESP, state.data_payload)
        auto_reply_ping = True
        auto_reply_data = True

    bus_max_payload = getattr(bus, "max_payload", active_sensors * 6)
    trace = TraceRecorder.from_config(
        active_sensors=active_sensors,
        trace_slots=trace_slots,
        trace_seconds=trace_seconds,
        trace_sample_hz=trace_sample_hz,
        trace_hold_s=trace_hold_s,
        bus_max_payload=bus_max_payload,
    )

    counters = {"rx": 0, "resp": 0, "event_tx": 0, "event_drop": 0, "event_retry": 0}
    event_engine = EventEngine(
        bus=bus,
        device_id=device_id,
        state=state,
        counters=counters,
        trace=trace,
        event_ack_timeout_s=event_ack_timeout_s,
        event_retry_max=event_retry_max,
        event_backoff_us=event_backoff_us,
        event_queue_max=event_queue_max,
        min_event_range=min_event_range,
        release_pct=release_pct,
        activation_pct=activation_pct,
        log_event_details=log_event_details,
    )

    scan_rate_tracker = ScanRateTracker(STATS_WINDOW_S)
    frame_dispatcher = FrameDispatcher(
        bus=bus,
        device_id=device_id,
        active_sensors=active_sensors,
        state=state,
        stats=stats,
        calibration=calibration,
        trace=trace,
        event_engine=event_engine,
        counters=counters,
        scan_rate_tracker=scan_rate_tracker,
        auto_reply_ping=auto_reply_ping,
        auto_reply_data=auto_reply_data,
        bus_max_payload=bus_max_payload,
    )
    usb_console = USBConsole(
        active_sensors=active_sensors,
        state=state,
        stats=stats,
        calibration=calibration,
    )

    if usb_verbose:
        if cal_status_msg:
            print(cal_status_msg)
        usb_console.print_table()

    scan_count = 0
    last_log_scan_count = 0
    last_log = time.monotonic()
    next_trace_cleanup = time.monotonic() + 0.5
    while True:
        now = time.monotonic()
        scanner.scan_into(state.scan_buffer, on_sample=event_engine.process_sample)
        scan_count += 1
        stats.update(state.scan_buffer, state.sensor_disabled)
        refresh_data_payload(state.data_payload, state.scan_buffer, state.sensor_disabled, stats)
        scan_rate_tracker.update(now)
        frame_dispatcher.handle_frames()
        event_engine.service(now)

        if trace.enabled and now >= next_trace_cleanup:
            trace.cleanup(now)
            next_trace_cleanup = now + 0.5

        now = time.monotonic()
        if usb_verbose and serial_poll_requested() and usb_connected():
            usb_console.handle_input()
        if now - last_log >= LOG_INTERVAL_S:
            elapsed = max(now - last_log, 0.001)
            scan_rate = (scan_count - last_log_scan_count) / elapsed
            print(
                f"[RS485 node {device_id}] last {LOG_INTERVAL_S:.0f}s: "
                f"rx={counters['rx']} resp={counters['resp']} scan_hz={scan_rate:.1f} "
                f"evt_tx={counters['event_tx']} evt_drop={counters['event_drop']} "
                f"evt_retry={counters['event_retry']}"
            )
            counters["rx"] = 0
            counters["resp"] = 0
            counters["event_tx"] = 0
            counters["event_drop"] = 0
            counters["event_retry"] = 0
            last_log_scan_count = scan_count
            last_log = now


if __name__ == "__main__":
    main()
