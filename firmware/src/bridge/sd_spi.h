// microSD card in SPI mode on the bridge boards (001 / 001D), plus the FatFs
// diskio glue for physical drive 0 (implemented in sd_spi.c). Blocking
// transfers; called only from core 1 (the recorder), which owns SPI1 once
// the boot-time sensor probe has found no banks.
#ifndef PHOTON_SD_SPI_H
#define PHOTON_SD_SPI_H

#include <stdbool.h>
#include <stdint.h>

// Full SPI-mode init sequence (CMD0/CMD8/ACMD41/CMD58, CSD read). Returns
// false when no card answers; the recorder retries periodically.
bool sd_spi_init(void);

bool sd_spi_ready(void);
uint32_t sd_spi_sector_count(void);

// 512-byte sectors. Any failure marks the card gone (sd_spi_ready() false)
// so the recorder re-mounts from scratch.
bool sd_spi_read(uint32_t lba, uint8_t *buf, uint32_t count);
bool sd_spi_write(uint32_t lba, const uint8_t *buf, uint32_t count);
bool sd_spi_sync(void);

#endif
