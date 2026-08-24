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

// Flash size is deliberately set to the SMALLER of the two parts we ship on,
// so ONE UF2 runs on both:
//   RP2350A + external Winbond W25Q128JVxQ ... 16 MB
//   RP2354A with 2 MB stacked W25Q16JVWI   ...  2 MB
// The firmware image is ~82 kB, so 2 MB is ample; the only size-dependent
// item is the config store, which anchors to the top of flash (see
// PHOTON_CONFIG_FLASH_OFFS) and therefore must live inside 2 MB to be valid
// on both. config_store.c additionally reads the old 16 MB location so that
// boards previously flashed with a 16 MB build keep their node id and
// calibration across the upgrade.
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#define PICO_FLASH_SPI_CLKDIV 2

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

// Deliberately no PICO_DEFAULT_UART / _LED_PIN / _I2C / _SPI: GPIO25 is
// RS-485 TERM on the sensor board, and every peripheral is claimed
// explicitly in board_config.h.

#endif
