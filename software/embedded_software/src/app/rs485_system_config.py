"""Single source of truth for the deployed RS-485 system configuration."""

from __future__ import annotations

SENSOR_NODE_ID_PATH = "/sensor_node_id"

UART_BAUD = 2_000_000
MAX_SENSORS = 32
BOARD_PAIR_SIZE = 2
MIDI_LOW_FF = 29
MIDI_HIGH_F = 89
BASE_DISABLED_SENSORS = (31, 62, 63)

# MIDI channels are zero-based internally; 1 is user-facing MIDI channel 2.
MIDI_CHANNEL_BASE = 1
SENSOR_NODE_DEVICE_IDS = (1, 2)

HOST_CONFIG = {
    "event_mode": True,
    "min_range": 170,
    "velocity_min_ms": 8.0,
    "velocity_max_ms": 100.0,
    "velocity_curve": 2.54,
    "log_velocity_details": False,
    "send_midi": True,
    "trace_sample_hz": 100,
    "trace_seconds": 10,
    "trace_fetch_on_off": False,
    "log_events": False,
    "log_midi_events": True,
}

SENSOR_NODE_CONFIG = {
    "device_id": 1,
    "active_sensors": 31,
    "rs485_driver_termination_enabled": False,
    "uart_baud": UART_BAUD,
    "settle_us": 60,
    "samples_per_channel": 1,
    "osr_mode": 3,
    "sensors_per_bank": 4,
    "sensor_spi_baudrate": 20_000_000,
    "sensor_spi_mode": 0,
    # Per-bank TLA2518 <- sensor slot mapping:
    # slot0 -> AIN7 + GPIO6, slot1 -> AIN5 + GPIO4,
    # slot2 -> AIN3 + GPIO2, slot3 -> AIN1 + GPIO0
    "sensor_adc_channels": [7, 5, 3, 1],
    "sensor_enable_gpio_bits": [6, 4, 2, 0],
    "bank_spi_bus": [0, 0, 0, 0, 1, 1, 1, 1],
    "event_mode": True,
    "event_ack_timeout_s": 0.012,
    "event_retry_max": 4,
    "event_backoff_us": [100, 1000],
    "event_queue_max": 64,
    "min_sensor_dynamic_range": 170,
    "strike_pct": 60,
    "release_pct": 40,
    "activation_pct": 3,
    "boot_auto_disable_enabled": False,
    "boot_disable_above": 3_000,
    "disabled_sensors": [],
    "trace_slots": 10,
    "trace_seconds": 10,
    "trace_sample_hz": 100,
    "trace_hold_s": 1.2,
    "payload_refresh_every_sweeps": 25,
    "log_system_status_on_boot": True,
    "system_status_probe_bank": 0,
    "usb_table_interval_s": 5.0,
    "usb_verbose": True,
    "log_event_details": True,
    "pins": {
        "uart_tx": "GPIO22",
        "uart_rx": "GPIO23",
        "rs485_de": "GPIO24",
        "rs485_term_control": "GPIO25",
        "spi0_sclk": "GPIO10",
        "spi0_mosi": "GPIO11",
        "spi0_miso": "GPIO8",
        "spi1_sclk": "GPIO2",
        "spi1_mosi": "GPIO3",
        "spi1_miso": "GPIO0",
        "bank_cs": [
            "GPIO21",  # bank 0
            "GPIO20",  # bank 1
            "GPIO19",  # bank 2
            "GPIO15",  # bank 3
            "GPIO1",   # bank 4
            "GPIO7",   # bank 5
            "GPIO5",   # bank 6
            "GPIO6",   # bank 7
        ],
    },
}

SENSOR_NODE_OVERRIDES = {
    2: {
        "disabled_sensors": [30],
    },
}


def _clone(value):
    if isinstance(value, dict):
        return {key: _clone(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_clone(item) for item in value]
    if isinstance(value, tuple):
        return tuple(_clone(item) for item in value)
    return value


def _deep_merge(base, override):
    if isinstance(base, dict) and isinstance(override, dict):
        merged = _clone(base)
        for key, value in override.items():
            merged[key] = _deep_merge(base.get(key), value)
        return merged
    if override is None:
        return _clone(base)
    return _clone(override)


def sensors_per_manual() -> int:
    return MAX_SENSORS * BOARD_PAIR_SIZE


def total_sensors() -> int:
    return len(SENSOR_NODE_DEVICE_IDS) * MAX_SENSORS


def manual_count() -> int:
    return max(1, (len(SENSOR_NODE_DEVICE_IDS) + BOARD_PAIR_SIZE - 1) // BOARD_PAIR_SIZE)


def default_disabled_sensors() -> tuple[int, ...]:
    per_manual = sensors_per_manual()
    total = total_sensors()
    return tuple(
        base + (manual_idx * per_manual)
        for manual_idx in range(manual_count())
        for base in BASE_DISABLED_SENSORS
        if base + (manual_idx * per_manual) < total
    )


def parse_disabled_sensors(value, total: int, *, base_disabled=()) -> set[int]:
    disabled = set()
    for entry in base_disabled:
        try:
            idx = int(entry)
        except Exception:
            continue
        if 0 <= idx < total:
            disabled.add(idx)
    if isinstance(value, (list, tuple)):
        for entry in value:
            try:
                idx = int(entry)
            except Exception:
                continue
            if 0 <= idx < total:
                disabled.add(idx)
    return disabled


def rs485_host_config() -> dict:
    cfg = _clone(HOST_CONFIG)
    cfg["disabled_sensors"] = list(default_disabled_sensors())
    return cfg


def read_sensor_node_id(path: str = SENSOR_NODE_ID_PATH, default: int = 1) -> int:
    try:
        with open(path, "r") as handle:
            value = handle.read().strip()
    except OSError:
        return default
    try:
        return int(value)
    except ValueError:
        return default


def rs485_sensor_node_config(device_id: int | None = None) -> dict:
    if device_id is None:
        device_id = read_sensor_node_id()
    cfg = _clone(SENSOR_NODE_CONFIG)
    cfg["device_id"] = int(device_id)
    return _deep_merge(cfg, SENSOR_NODE_OVERRIDES.get(int(device_id), {}))

