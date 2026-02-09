"""Hardware setup for the RS-485 main host."""

from __future__ import annotations

try:
    import busio
    import digitalio
except Exception:  # pragma: no cover - host envs don't have CircuitPython modules
    busio = None  # type: ignore
    digitalio = None  # type: ignore

try:
    import photon_rs485
except Exception:  # pragma: no cover - host envs don't have CircuitPython modules
    photon_rs485 = None  # type: ignore

from app.rs485_bus import FRAME_HEADER_LEN, FRAME_TRAILER_LEN, RS485Bus

from .constants import TX_ENABLE_DELAY_S, UART_BAUD, MAX_SENSORS
from .pins import RS485_DE_PIN, UART_RX_PIN, UART_TX_PIN


def _require_hw(name: str, value) -> None:
    if value is None:
        raise RuntimeError(f"{name} requires CircuitPython hardware modules.")


def setup_output(pin, *, value=False):
    _require_hw("digitalio", digitalio)
    _require_hw("pin", pin)
    dio = digitalio.DigitalInOut(pin)
    dio.direction = digitalio.Direction.OUTPUT
    dio.value = bool(value)
    return dio


def setup_uart():
    _require_hw("busio", busio)
    _require_hw("UART_TX_PIN", UART_TX_PIN)
    _require_hw("UART_RX_PIN", UART_RX_PIN)
    return busio.UART(
        UART_TX_PIN,
        UART_RX_PIN,
        baudrate=UART_BAUD,
        timeout=0,
        receiver_buffer_size=65535,
    )


def setup_rs485():
    max_payload = MAX_SENSORS * 6
    min_rx_buffer_size = FRAME_HEADER_LEN + max_payload + FRAME_TRAILER_LEN
    if photon_rs485 is None:
        uart = setup_uart()
        rs485_de = setup_output(RS485_DE_PIN, value=False)
        bus = RS485Bus(
            uart,
            rs485_de,
            baudrate=UART_BAUD,
            tx_enable_delay_s=TX_ENABLE_DELAY_S,
            max_payload=max_payload,
        )
        return bus, False

    tx_delay_us = int(TX_ENABLE_DELAY_S * 1_000_000)
    rx_buffer_size = max(65535, min_rx_buffer_size)
    bus = photon_rs485.RS485(
        UART_TX_PIN,
        UART_RX_PIN,
        RS485_DE_PIN,
        baudrate=UART_BAUD,
        device_id=0,
        tx_enable_delay_us=tx_delay_us,
        rx_buffer_size=rx_buffer_size,
        max_payload=max_payload,
    )
    return bus, True
