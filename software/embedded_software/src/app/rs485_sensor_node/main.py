"""RS-485 sensor node (run mode 'rs485-sensor-node')."""

from __future__ import annotations

import os
import time

from app import nvm_flags
from photon_rs485 import (
    FRAME_TYPE_DATA_REQ,
    FRAME_TYPE_DATA_RESP,
    FRAME_TYPE_PING,
    FRAME_TYPE_PONG,
)

from .calibration import CalibrationManager, print_cal_save_unavailable_warning
from app.rs485_system_config import parse_disabled_sensors, rs485_sensor_node_config
from .constants import (
    CALIBRATION_PATH,
    LOG_INTERVAL_S,
    STATS_WINDOW_S,
)
from .events import EventEngine
from .hardware_setup import setup_outputs, setup_rs485, setup_scanner
from .state import NodeState
from .stats import ScanRateTracker, StatsTracker, refresh_data_payload
from .trace import TraceRecorder
from app.utils import serial_poll_requested, usb_connected, usb_console_connected
from .usb_console import USBConsole
from .protocol import FrameDispatcher
from .runtime import microcontroller

def _clamp_pct(value: int, default: int) -> int:
    try:
        value = int(value)
    except Exception:
        return default
    return max(0, min(100, value))


def _refresh_scan_payload(state: NodeState, stats: StatsTracker) -> None:
    refresh_data_payload(state.data_payload, state.scan_buffer, state.sensor_disabled, stats)


def _scan_has_nonzero_values(state: NodeState) -> bool:
    active_count = 0
    for sensor_idx in range(len(state.latest_values)):
        if state.sensor_disabled[sensor_idx]:
            continue
        active_count += 1
        if int(state.scan_buffer[sensor_idx]) > 0:
            return True
    # If all sensors are disabled, don't treat that as a startup all-zero fault.
    return active_count == 0


def _log_startup_flat_bank_check(state: NodeState, active_sensors: int, sensors_per_bank: int) -> None:
    if active_sensors <= 0 or sensors_per_bank <= 0:
        return
    bank_count = (active_sensors + sensors_per_bank - 1) // sensors_per_bank
    for bank in range(bank_count):
        start = bank * sensors_per_bank
        end = min(start + sensors_per_bank, active_sensors)
        if end <= start:
            continue
        first = int(state.latest_values[start])
        all_same = True
        for idx in range(start + 1, end):
            if int(state.latest_values[idx]) != first:
                all_same = False
                break
        if all_same:
            print(
                f"[WARN] Flatline bank candidate bank={bank} sensors={start}-{end - 1} value={first}"
            )


def _attempt_scanner_reinit(scanner, *, max_reinit_attempts: int = 3) -> bool:
    for attempt in range(1, max_reinit_attempts + 1):
        try:
            recovered = bool(scanner.reinit_c_backend())
        except Exception as exc:
            print(f"[WARN] C SPI driver reinit raised {exc.__class__.__name__}: {exc}")
            recovered = False
        if recovered:
            print(f"[INFO] C SPI driver recovered after {attempt} attempt(s).")
            return True
        if attempt < max_reinit_attempts:
            time.sleep(0.05)
    print(f"[WARN] C SPI driver reinit failed after {max_reinit_attempts} attempts.")
    return False


def _rescan_after_reinit(scanner, state: NodeState, stats: StatsTracker, *, on_scan) -> bool:
    time.sleep(0.05)
    scanner.scan_all_sensors(on_scan=on_scan)
    stats.update(state.scan_buffer, state.sensor_disabled)
    _refresh_scan_payload(state, stats)
    if _scan_has_nonzero_values(state):
        print("[CHK] Payload recovered after C SPI driver reinit.")
        return True
    print("[WARN] Payload still all zeros after C SPI driver reinit; doing MCU reset.")
    return False


def _handle_startup_zero_payload_check(
    *,
    pending: bool,
    scan_start_time: float,
    scanner,
    state: NodeState,
    stats: StatsTracker,
    on_scan,
) -> tuple[bool, bool]:
    """Return (still_pending, skip_rest_of_loop)."""
    if not pending:
        return False, False
    if (time.monotonic() - scan_start_time) < 1.0:
        return True, False

    _refresh_scan_payload(state, stats)
    if _scan_has_nonzero_values(state):
        return False, False

    print("[CHK] Payload all zeros; attempting C SPI driver reinit before MCU reset.")
    # As of Feb 27, 2026, on CircuitPython 10.1.3, SPI can fail
    # to initialize on first bring-up and may need a few retries.
    recovered = _attempt_scanner_reinit(scanner, max_reinit_attempts=3)
    if recovered and _rescan_after_reinit(
        scanner,
        state,
        stats,
        on_scan=on_scan,
    ):
        return False, True
    if not recovered:
        print("[WARN] C SPI driver reinit failed; doing MCU reset.")

    time.sleep(0.5)
    if microcontroller is not None:
        microcontroller.reset()
        return False, True
    print("[WARN] MCU reset unavailable; continuing.")
    return False, False


def main() -> None:
    cfg = rs485_sensor_node_config()
    device_id = int(cfg["device_id"])
    active_sensors = int(cfg["active_sensors"])
    settle_us = int(cfg.get("settle_us", 17))
    event_ack_timeout_s = float(cfg.get("event_ack_timeout_s", 0.005))
    event_retry_max = int(cfg.get("event_retry_max", 4))
    event_backoff_us = cfg.get("event_backoff_us", [50, 200])
    event_queue_max = int(cfg.get("event_queue_max", 64))
    # min_sensor_dynamic_range base value is calibrated for OSR=0 (12-bit).
    # OSR accumulates 2^osr_mode samples, scaling the output range accordingly.
    osr_mode = int(cfg.get("osr_mode", 0))
    min_sensor_dynamic_range = int(cfg.get("min_sensor_dynamic_range", 170)) * (1 << osr_mode)
    strike_pct_default = int(cfg.get("strike_pct", 60))
    release_pct = int(cfg.get("release_pct", 40))
    activation_pct = int(cfg.get("activation_pct", 3))
    boot_auto_disable_enabled = bool(cfg.get("boot_auto_disable_enabled", False))
    boot_disable_above = int(cfg.get("boot_disable_above", 3_000))
    trace_slots = int(cfg.get("trace_slots", 10))
    trace_seconds = int(cfg.get("trace_seconds", 10))
    trace_sample_hz = int(cfg.get("trace_sample_hz", 100))
    trace_hold_s = float(cfg.get("trace_hold_s", 1.2))
    payload_refresh_every_sweeps = int(cfg.get("payload_refresh_every_sweeps", 25))
    sensor_spi_baudrate = int(cfg.get("sensor_spi_baudrate", 20_000_000))
    log_system_status_on_boot = bool(cfg.get("log_system_status_on_boot", True))
    system_status_probe_bank = int(cfg.get("system_status_probe_bank", 0))
    usb_verbose = bool(cfg.get("usb_verbose", True))
    # Event details follow live USB connection state.
    log_event_details = usb_connected()
    disabled_sensors_cfg = parse_disabled_sensors(cfg.get("disabled_sensors"), active_sensors)
    reset_cal_on_boot = nvm_flags.reset_calibration_on_boot()

    strike_pct_default = _clamp_pct(strike_pct_default, 60)
    release_pct = _clamp_pct(release_pct, 40)
    activation_pct = _clamp_pct(activation_pct, 3)
    if payload_refresh_every_sweeps < 1:
        payload_refresh_every_sweeps = 1
    if sensor_spi_baudrate < 100_000:
        sensor_spi_baudrate = 100_000

    setup_outputs(cfg)
    scanner = setup_scanner(cfg, active_sensors, settle_us)
    if log_system_status_on_boot:
        if system_status_probe_bank < 0:
            system_status_probe_bank = 0
        try:
            scanner.arm_startup_system_status_probe(system_status_probe_bank)
        except Exception as exc:
            print(
                f"[WARN] SYSTEM_STATUS startup reset failed on bank{system_status_probe_bank}: {exc}"
            )
    bus = setup_rs485(cfg, device_id, active_sensors * 6)

    if usb_verbose:
        time.sleep(0.5)
    print("")
    print(f"[BOOT] Board: {os.uname().machine}")
    print(f"[BOOT] CircuitPython build: {os.uname().version}")
    print("")
    print("")
    print(".*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*")
    print(".*.*.*.*  PHOTON RS485 Sensor Node  .*.*.*")
    print(".*.   https://github.com/w4iei/photon   .*")
    print(".*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*")
    print("Creator: Noah Jaffe")
    print("")
    print("[BOOT] RS485 Sensor Node")
    print(f"[CFG] Device ID: {device_id}")
    print(f"[CFG] Active sensors: {active_sensors}")
    print("[CFG] Scan rate: unthrottled")
    print(f"[CFG] Settle time before sensor ADC capture: {settle_us} us")
    print(f"[CFG] RS485 UART baud rate: {cfg['uart_baud'] / 1_000_000.0:.2f} MHz")
    print(f"[CFG] RS485 driver termination (internal to IC): {bool(cfg.get('rs485_driver_termination_enabled', False))}")
    print("[CFG] RS485 driver: photon_rs485 (C driver)")
    print("[CFG] Sensor scan: TLA2518 SPI (C driver)")
    print(f"[CFG] Sensor SPI baud: {sensor_spi_baudrate / 1_000_000.0:.1f} MHz")
    print("[CFG] Event mode: on")
    print(f"[CFG] Startup auto-disable: {'on' if boot_auto_disable_enabled else 'off'}")
    print(f"[CFG] Payload refresh interval: every {payload_refresh_every_sweeps} scans")
    print("[CFG] NVM flags:", nvm_flags.describe_flags())
    if reset_cal_on_boot and not nvm_flags.is_usb_drive_disabled():
        print_cal_save_unavailable_warning()
    if usb_verbose:
        print("[INFO] USB serial commands:")
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

    state = NodeState(
        active_sensors,
        strike_pct_default,
        scan_buffer=scanner.readings_buffer,
    )
    stats = StatsTracker(active_sensors)
    calibration = CalibrationManager(
        device_id=device_id,
        active_sensors=active_sensors,
        boot_auto_disable_enabled=boot_auto_disable_enabled,
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
    _refresh_scan_payload(state, stats)

    bus.add_auto_reply(FRAME_TYPE_PING, FRAME_TYPE_PONG, b"")
    bus.add_auto_reply(FRAME_TYPE_DATA_REQ, FRAME_TYPE_DATA_RESP, state.data_payload)

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
        min_sensor_dynamic_range=min_sensor_dynamic_range,
        release_pct=release_pct,
        activation_pct=activation_pct,
        log_event_details=log_event_details,
    )
    scan_on_scan = event_engine.process_scan

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
            if cal_status_msg.startswith("Loaded calibration"):
                print(f"[READY] {cal_status_msg}")
            else:
                print(f"[WARN] {cal_status_msg}")
        print("[INFO] Press Enter over USB serial to print the full sensor table.")
    print(
        f"[READY] node={device_id} sensors={active_sensors} "
        f"event=on rs485=photon_rs485"
    )

    scan_count = 0
    last_log_scan_count = 0
    last_log = time.monotonic()
    next_trace_cleanup = time.monotonic() + 0.5
    scan_start_time = time.monotonic()
    startup_zero_payload_check_pending = True
    while True:
        event_engine.log_event_details = usb_connected()
        now = time.monotonic()
        scanner.scan_all_sensors(on_scan=scan_on_scan)
        scan_count += 1
        stats.update(state.scan_buffer, state.sensor_disabled)
        payload_refresh_due = (scan_count % payload_refresh_every_sweeps) == 0
        if payload_refresh_due:
            _refresh_scan_payload(state, stats)
        startup_zero_payload_check_pending, skip_rest_of_loop = _handle_startup_zero_payload_check(
            pending=startup_zero_payload_check_pending,
            scan_start_time=scan_start_time,
            scanner=scanner,
            state=state,
            stats=stats,
            on_scan=scan_on_scan,
        )
        if skip_rest_of_loop:
            continue
        scan_rate_tracker.update(now)
        frame_dispatcher.handle_frames()
        event_engine.service(now)

        if trace.enabled and now >= next_trace_cleanup:
            trace.cleanup(now)
            next_trace_cleanup = now + 0.5

        now = time.monotonic()
        if usb_verbose and serial_poll_requested() and usb_console_connected():
            usb_console.handle_input()
        if now - last_log >= LOG_INTERVAL_S:
            elapsed = max(now - last_log, 0.001)
            scan_rate = (scan_count - last_log_scan_count) / elapsed
            print(
                f"[STAT] node={device_id} last {LOG_INTERVAL_S:.0f}s: "
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
