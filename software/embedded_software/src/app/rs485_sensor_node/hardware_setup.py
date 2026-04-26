"""Hardware setup for the RS-485 sensor node."""

from __future__ import annotations

import time

import photon_rs485

from app import hardware
from app.sensor_scanner import Scanner as SensorScanner

from .constants import SENSORS_PER_BANK
from .runtime import board


def resolve_pin(name: str):
    cleaned = name.strip().upper().replace("GPIO", "GP")
    return getattr(board, cleaned)


def setup_outputs(cfg: dict):
    return hardware.claim_output(
        resolve_pin(cfg["pins"]["rs485_term_control"]),
        value=bool(cfg["rs485_driver_termination_enabled"]),
    )


def setup_scanner(cfg: dict, total_sensors: int, settle_us: int) -> SensorScanner:
    pins = cfg["pins"]
    bank_cs_pins = [resolve_pin(name) for name in pins["bank_cs"]]
    spi0_pins = tuple(resolve_pin(pins[f"spi0_{p}"]) for p in ["sclk", "mosi", "miso"])
    spi1_pins = tuple(resolve_pin(pins[f"spi1_{p}"]) for p in ["sclk", "mosi", "miso"])

    # Load and validate config
    samples_per_channel = max(1, int(cfg.get("samples_per_channel", 1)))
    osr_mode = max(0, min(7, int(cfg.get("osr_mode", 0))))
    sensors_per_bank = max(1, int(cfg.get("sensors_per_bank", SENSORS_PER_BANK)))
    spi_baudrate = max(100_000, int(cfg.get("sensor_spi_baudrate", 20_000_000)))
    spi_mode = max(0, min(3, int(cfg.get("sensor_spi_mode", 0))))
    bank_spi_bus = cfg.get("bank_spi_bus", [0 if i < 4 else 1 for i in range(len(bank_cs_pins))])

    time.sleep(0.2)  # Scanner powerup delay: 200ms

    return SensorScanner(
        [spi0_pins, spi1_pins],
        bank_cs_pins,
        settle_us=settle_us,
        samples_per_channel=samples_per_channel,
        sensors_per_bank=sensors_per_bank,
        total_sensors=total_sensors,
        bank_spi_bus=bank_spi_bus,
        sensor_adc_channels=cfg.get("sensor_adc_channels", [7, 5, 3, 1]),
        sensor_enable_gpio_bits=cfg.get("sensor_enable_gpio_bits", [6, 4, 2, 0]),
        spi_baudrate=spi_baudrate,
        spi_mode=spi_mode,
        osr_mode=osr_mode,
    )


def setup_rs485(cfg: dict, device_id: int, max_payload: int):
    min_rx_buffer_size = photon_rs485.FRAME_HEADER_LEN + max_payload + photon_rs485.FRAME_TRAILER_LEN
    rx_buffer_size = max(min_rx_buffer_size, min(0xFFFF, int(cfg.get("rx_buffer_size", 16384))))

    bus = photon_rs485.RS485(
        resolve_pin(cfg["pins"]["uart_tx"]),
        resolve_pin(cfg["pins"]["uart_rx"]),
        resolve_pin(cfg["pins"]["rs485_de"]),
        baudrate=int(cfg["uart_baud"]),
        device_id=device_id,
        tx_enable_delay_us=25,
        rx_buffer_size=rx_buffer_size,
        max_payload=max_payload,
    )
    return bus
