"""Single-sensor high-rate trace capture.

Uses the current RS-485 sensor-node configuration and scanner backend without
initializing any RS-485 transport.
"""

from __future__ import annotations

import time

from app.rs485_sensor_node.hardware_setup import setup_scanner
from app.rs485_system_config import parse_disabled_sensors, rs485_sensor_node_config
from app.sensor_calibration import extract_node_calibration, load_calibration_file
from app.utils import log_info, read_serial_input, serial_poll_requested, sleep_us


CALIBRATION_PATH = "/config/rs485_sensor_node_cal.json"

SENSOR_VALUE_MAX = 65535

SENSOR_IDX = 24
TARGET_HZ = 1000
CAPTURE_SECONDS = 3.0


def _clamp_pct(value: int, default: int) -> int:
    try:
        value = int(value)
    except Exception:
        return default
    return max(0, min(100, value))


def _load_calibration_for_sensor(device_id: int, active_sensors: int, sensor_idx: int):
    payload = load_calibration_file(CALIBRATION_PATH)
    if payload is None:
        raise RuntimeError(f"Calibration file missing at {CALIBRATION_PATH}")
    node_payload = extract_node_calibration(payload, device_id)
    if node_payload is None:
        raise RuntimeError(
            f"Calibration for device {device_id} not found in {CALIBRATION_PATH}"
        )
    mins = node_payload.get("min")
    maxs = node_payload.get("max")
    count = int(node_payload.get("active_sensors", active_sensors))
    if not isinstance(mins, list) or not isinstance(maxs, list):
        raise RuntimeError("Calibration payload missing min/max arrays")
    if sensor_idx >= len(mins) or sensor_idx >= len(maxs):
        raise RuntimeError("Calibration payload shorter than sensor index")
    min_v = int(mins[sensor_idx])
    max_v = int(maxs[sensor_idx])
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
    return min_v, max_v, count


def _wait_for_newline() -> None:
    pending = ""
    while True:
        if serial_poll_requested():
            pending += read_serial_input()
            if "\n" in pending or "\r" in pending:
                return
        time.sleep(0.01)


def _capture_fixed_window(
    scanner,
    *,
    sensor_idx: int,
    thr_on: int,
    thr_off: int,
    interval_ns: int,
    capture_seconds: float,
):
    if interval_ns <= 0:
        interval_ns = 1_000_000
    sample_count = max(1, int(capture_seconds * (1_000_000_000 // interval_ns)))
    values = [0] * sample_count
    start_ns = time.monotonic_ns()

    for i in range(sample_count):
        scheduled_ns = start_ns + (i * interval_ns)
        now_ns = time.monotonic_ns()
        remaining_ns = scheduled_ns - now_ns
        if remaining_ns > 1000:
            sleep_us(remaining_ns // 1000)
        values[i] = int(scanner.read_sensor(sensor_idx))

    print(
        "BEGIN_TRACE sensor=%d midi=None note=None polarity=NOR thr_on=%d thr_off=%d"
        % (sensor_idx, thr_on, thr_off)
    )
    for i, value in enumerate(values):
        t_s = (i * interval_ns) * 1e-9
        print(f"{t_s:.3f},{value}")
    print("END_TRACE")

    dur_s = capture_seconds
    est_fs = (sample_count - 1) / dur_s if dur_s > 0 and sample_count > 1 else 0.0
    log_info(
        f"TRACE done: samples={sample_count} duration={dur_s:.6f}s est_fs={est_fs:.1f}Hz"
    )


def main() -> None:
    cfg = rs485_sensor_node_config()
    device_id = int(cfg.get("device_id", 1))
    active_sensors = int(cfg.get("active_sensors", 31))
    settle_us = int(cfg.get("settle_us", 18))
    strike_pct = _clamp_pct(cfg.get("strike_pct", 60), 60)
    release_pct = _clamp_pct(cfg.get("release_pct", 40), 40)
    osr_mode = max(0, int(cfg.get("osr_mode", 0)))
    min_event_range = int(cfg.get("min_event_range", cfg.get("min_sensor_dynamic_range", 170)))
    min_event_range *= 1 << osr_mode
    disabled_sensors = parse_disabled_sensors(cfg.get("disabled_sensors"), active_sensors)

    if SENSOR_IDX < 0 or SENSOR_IDX >= active_sensors:
        raise ValueError(f"sensor_idx {SENSOR_IDX} exceeds active_sensors {active_sensors}")
    if SENSOR_IDX in disabled_sensors:
        log_info(f"WARNING: sensor {SENSOR_IDX} disabled by config; capture may not trigger.")

    min_v, max_v, cal_count = _load_calibration_for_sensor(device_id, active_sensors, SENSOR_IDX)
    if cal_count != active_sensors:
        log_info(
            f"Calibration active_sensors={cal_count} differs from config active_sensors={active_sensors}"
        )
    rng = max_v - min_v
    if rng <= 0:
        raise RuntimeError(f"Invalid calibration range: min={min_v} max={max_v}")
    if rng < min_event_range:
        log_info(
            f"WARNING: sensor {SENSOR_IDX} range {rng} below min_event_range {min_event_range}"
        )

    if release_pct > strike_pct:
        release_pct = strike_pct
    thr_on = min_v + (rng * strike_pct) // 100
    thr_off = min_v + (rng * release_pct) // 100

    target_hz = int(cfg.get("single_sensor_sample_hz", cfg.get("trace_sample_hz", TARGET_HZ)))
    if target_hz <= 0:
        target_hz = TARGET_HZ
    if target_hz > TARGET_HZ:
        target_hz = TARGET_HZ
    interval_ns = int(1_000_000_000 // target_hz) if target_hz > 0 else 0

    scanner = setup_scanner(cfg, active_sensors, settle_us)

    log_info(
        "Single-sensor trace: sensor=%d device_id=%d settle_us=%d sample_hz=%d"
        % (SENSOR_IDX, device_id, settle_us, target_hz)
    )
    log_info(
        "Calibration: min=%d max=%d rng=%d thr_on=%d thr_off=%d"
        % (min_v, max_v, rng, thr_on, thr_off)
    )
    log_info(
        "Scanner config: sensors_per_bank=%d osr_mode=%d"
        % (int(cfg.get("sensors_per_bank", 4)), osr_mode)
    )
    log_info(
        "Capture window: duration_s=%.3f interval_ns=%d"
        % (CAPTURE_SECONDS, interval_ns)
    )

    while True:
        log_info("Waiting for newline on USB serial to start capture.")
        _wait_for_newline()
        _capture_fixed_window(
            scanner,
            sensor_idx=SENSOR_IDX,
            thr_on=thr_on,
            thr_off=thr_off,
            interval_ns=interval_ns,
            capture_seconds=CAPTURE_SECONDS,
        )


if __name__ == "__main__":
    main()
