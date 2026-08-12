// Board header for PHOTON boards (sensor board 002 / main controller 001).
// One image serves every board; roles are chosen at runtime by capability
// probe (see src/main.c). Only invariants live here — all functional pin
// assignments are in src/board_config.h.
#ifndef _BOARDS_PHOTON_RP2350_H
#define _BOARDS_PHOTON_RP2350_H

#define PHOTON_RP2350_BOARD 1

// RP2350A (QFN-60), 12 MHz crystal. Slow-start crystal margin carried over
// from the CircuitPython board config (PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64).
#define PICO_RP2350A 1
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 64

// Winbond W25Q128JVxQ, 16 MB QSPI flash.
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#define PICO_FLASH_SPI_CLKDIV 2

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

// Deliberately no PICO_DEFAULT_UART / _LED_PIN / _I2C / _SPI: GPIO25 is
// RS-485 TERM on the sensor board, and every peripheral is claimed
// explicitly in board_config.h.

#endif
