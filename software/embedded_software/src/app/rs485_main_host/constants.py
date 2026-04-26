"""Constants for the RS-485 main host."""

from app.rs485_system_config import (
    BOARD_PAIR_SIZE,
    MAX_SENSORS,
    MIDI_CHANNEL_BASE,
    MIDI_HIGH_F,
    MIDI_LOW_FF,
    SENSOR_NODE_DEVICE_IDS,
    UART_BAUD,
    default_disabled_sensors,
    manual_count,
    sensors_per_manual,
    total_sensors,
)

SENSOR_VALUE_MAX = 65535
MIN_SENSOR_RANGE = 625

REQUEST_INTERVAL_S = 1 / 400  # 1/ Sample rate (hz) (not per sensor...)
STATS_INTERVAL_S = 5.0
STATS_RESPONSE_TIMEOUT_S = 0.3
DATA_RESPONSE_TIMEOUT_S = 0.2
PING_RESPONSE_TIMEOUT_S = 0.015  # 15ms wait for PONG after each PING
PING_INTERVAL_S = 0.25
SENSORS_PER_MANUAL = sensors_per_manual()
TOTAL_SENSORS = total_sensors()
MANUAL_COUNT = manual_count()
DEFAULT_DISABLED_SENSORS = default_disabled_sensors()
STARTUP_PING_DURATION_S = 2.0
DATA_TIMEOUT_S = 2.0
PRINT_INTERVAL_S = 5.0
HEALTH_CHECK_IDLE_S = 10.0
CAL_CMD_TIMEOUT_S = 1.5
CALIBRATION_PATH = "/config/rs485_sensor_node_cal.json"
TRACE_CHUNK_SAMPLES = 25
DATA_RECORD_BYTES = 4
MINMAX_RECORD_BYTES = 6

# Calibration frame types (not in photon_rs485 C driver)
FRAME_TYPE_CAL_CMD = ord("K")
FRAME_TYPE_CAL_ACK = ord("k")
CAL_CMD_RESET = 1
CAL_CMD_SAVE = 2
CAL_ACK_OK = 1
CAL_ACK_FAIL = 0
