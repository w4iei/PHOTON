// APS6404L QSPI PSRAM on QMI chip select 1 (PHOTON_PSRAM_CS), mapped at
// 0x11000000. Board support only: the PHOTON image does not call it yet.
// Verified on a main controller board 001, 2026-09-30: found on GPIO0, all
// 8 MB pass psram_test().
#ifndef PHOTON_PSRAM_H
#define PHOTON_PSRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Read the chip ID on PHOTON_PSRAM_CS, configure QMI window 1 for quad
// reads/writes. Returns the size in bytes (0 = not found).
size_t psram_init(void);
size_t psram_size(void);
int psram_cs_pin(void);           // -1 if not found
uint8_t psram_kgd(void);          // known-good-die byte from the ID (0x5D)

// Walk `bytes` of PSRAM with an address pattern. Returns the number of bad
// words (0 = pass).
uint32_t psram_test(size_t bytes);

#endif
