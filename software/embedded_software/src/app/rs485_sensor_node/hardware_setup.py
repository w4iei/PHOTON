"""Hardware setup for the RS-485 sensor node."""

from __future__ import annotations

from app.helpers import hardware
from .sensor_scanner import Scanner as SensorScanner
from app.rs485_bus import FRAME_HEADER_LEN, FRAME_TRAILER_LEN, RS485Bus

from .constants import SENSORS_PER_BANK
from .runtime import board, busio, photon_rs485


def resolve_pin(name: str):
    if board is None:
        raise RuntimeError("Pin resolution requires CircuitPython's 'board' module.")
    cleaned = name.strip().upper().replace("GPIO", "GP")
    return getattr(board, cleaned)


def setup_uart(cfg: dict, rx_buffer_size: int):
    uart = busio.UART(
        resolve_pin(cfg["pins"]["uart_tx"]),
        resolve_pin(cfg["pins"]["uart_rx"]),
        baudrate=cfg["uart_baud"],
        timeout=0,
        receiver_buffer_size=rx_buffer_size,
    )
    return uart


def setup_outputs(cfg: dict):
    term_pin = hardware.claim_output(
        resolve_pin(cfg["pins"]["rs485_left_term"]),
        value=bool(cfg["left_term"]),
    )
    return term_pin


def setup_scanner(cfg: dict, total_sensors: int, settle_us: int) -> SensorScanner:
    enable_pins = [resolve_pin(name) for name in cfg["pins"]["bank_en"]]
    sel0_pin = resolve_pin(cfg["pins"]["y_sel_0"])
    sel1_pin = resolve_pin(cfg["pins"]["y_sel_1"])
    adc_pin = resolve_pin(cfg["pins"]["adc"])
    samples_per_channel = int(cfg.get("samples_per_channel", 5))
    sensors_per_bank = int(cfg.get("sensors_per_bank", SENSORS_PER_BANK))
    force_python_scan = bool(cfg.get("force_python_scan", False))
    if samples_per_channel < 1:
        samples_per_channel = 1
    if sensors_per_bank < 1:
        sensors_per_bank = SENSORS_PER_BANK
    return SensorScanner(
        enable_pins,
        sel0_pin,
        sel1_pin,
        adc_pin,
        settle_us=settle_us,
        samples_per_channel=samples_per_channel,
        sensors_per_bank=sensors_per_bank,
        total_sensors=total_sensors,
        use_c=None if not force_python_scan else False,
    )


def setup_rs485(cfg: dict, device_id: int, max_payload: int):
    min_rx_buffer_size = FRAME_HEADER_LEN + max_payload + FRAME_TRAILER_LEN
    rx_buffer_size = int(cfg.get("rx_buffer_size", 16384))
    if rx_buffer_size < min_rx_buffer_size:
        rx_buffer_size = min_rx_buffer_size
    if rx_buffer_size > 0xFFFF:
        rx_buffer_size = 0xFFFF
    if photon_rs485 is None:
        uart = setup_uart(cfg, rx_buffer_size)
        de_pin = hardware.claim_output(resolve_pin(cfg["pins"]["rs485_de"]), value=False)
        bus = RS485Bus(
            uart,
            de_pin,
            baudrate=int(cfg["uart_baud"]),
            tx_enable_delay_s=float(cfg.get("tx_enable_delay_s", 0.000025)),
            max_payload=max_payload,
        )
        return bus, False

    tx_pin = resolve_pin(cfg["pins"]["uart_tx"])
    rx_pin = resolve_pin(cfg["pins"]["uart_rx"])
    de_pin = resolve_pin(cfg["pins"]["rs485_de"])
    tx_delay_us = int(float(cfg.get("tx_enable_delay_s", 0.000025)) * 1_000_000)
    bus = photon_rs485.RS485(
        tx_pin,
        rx_pin,
        de_pin,
        baudrate=int(cfg["uart_baud"]),
        device_id=device_id,
        tx_enable_delay_us=tx_delay_us,
        rx_buffer_size=rx_buffer_size,
        max_payload=max_payload,
    )
    return bus, True
