"""Pin definitions for the RS-485 main host."""

import microcontroller

UART_TX_PIN = microcontroller.pin.GPIO4
UART_RX_PIN = microcontroller.pin.GPIO5
RS485_DE_PIN = microcontroller.pin.GPIO1
RS485_TERM_PIN = microcontroller.pin.GPIO18
