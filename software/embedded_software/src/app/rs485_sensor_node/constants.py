"""Constants for the RS-485 sensor node."""

CONFIG_PATH = "/config/rs485_sensor_node.json"
CALIBRATION_PATH = "/config/rs485_sensor_node_cal.json"

DEFAULT_CONFIG = {
    "device_id": 1,
    "active_sensors": 31,
    "left_term": False,
    "uart_baud": 2_000_000,
    "settle_us": 70,
    "samples_per_channel": 1,
    "sensors_per_bank": 4,
    "tx_enable_delay_s": 0.000025,
    "force_python_scan": False,
    "event_mode": True,
    "event_ack_timeout_s": 0.005,
    "event_retry_max": 4,
    "event_backoff_us": [100, 1000],
    "event_queue_max": 64,
    "min_event_range": 170,
    "strike_pct": 60,
    "release_pct": 40,
    "activation_pct": 3,
    "boot_disable_above": 3_000,  # Disable sensors above this baseline value.
    "disabled_sensors": [],
    "trace_slots": 10,
    "trace_seconds": 10,
    "trace_sample_hz": 100,
    "trace_hold_s": 1.2,
    "usb_table_interval_s": 5.0,
    "usb_verbose": True,
    "log_event_details": True,
    "pins": {
        "uart_tx": "GPIO22",
        "uart_rx": "GPIO23",
        "rs485_de": "GPIO24",
        "rs485_left_term": "GPIO25",
        "y_sel_0": "GPIO13",
        "y_sel_1": "GPIO14",
        "bank_en": [
            "GPIO19",  # bank 0
            "GPIO20",  # bank 1
            "GPIO21",  # bank 2
            "GPIO15",  # bank 3
            "GPIO4",   # bank 4
            "GPIO7",   # bank 5
            "GPIO5",   # bank 6
            "GPIO6",   # bank 7
        ],
        "adc": "A0",
    },
}

STATS_WINDOW_S = 3.0
LOG_INTERVAL_S = 5.0
POLARITY_NORMAL = 0
POLARITY_REVERSED = 1
DATA_RECORD_BYTES = 4
MINMAX_RECORD_BYTES = 6
STD_WINDOW = 100
EVENT_STATE_OFF = 0
EVENT_STATE_ON = 1
TRACE_CHUNK_SAMPLES = 25
VELOCITY_WINDOW_PCT = 20
ADJACENT_GUARD_PCT = 30
SENSORS_PER_BANK = 4
SENSOR_VALUE_MAX = 4095
CAL_CMD_RESET = 1
CAL_CMD_SAVE = 2
CAL_ACK_OK = 1
CAL_ACK_FAIL = 0
