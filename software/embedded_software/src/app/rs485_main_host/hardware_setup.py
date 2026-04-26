"""Hardware setup for the RS-485 main host."""

from __future__ import annotations

import digitalio
import photon_rs485

from .constants import UART_BAUD, MAX_SENSORS
from .pins import RS485_DE_PIN, UART_RX_PIN, UART_TX_PIN


def setup_output(pin, *, value=False):
    dio = digitalio.DigitalInOut(pin)
    dio.direction = digitalio.Direction.OUTPUT
    dio.value = bool(value)
    return dio


def setup_rs485():
    max_payload = MAX_SENSORS * 6
    min_rx_buffer_size = photon_rs485.FRAME_HEADER_LEN + max_payload + photon_rs485.FRAME_TRAILER_LEN
    rx_buffer_size = max(65535, min_rx_buffer_size)
    bus = photon_rs485.RS485(
        UART_TX_PIN,
        UART_RX_PIN,
        RS485_DE_PIN,
        baudrate=UART_BAUD,
        device_id=0,
        tx_enable_delay_us=25,
        rx_buffer_size=rx_buffer_size,
        max_payload=max_payload,
    )
    return bus
