#ifndef PHOTON_CRC32_H
#define PHOTON_CRC32_H

#include <stddef.h>
#include <stdint.h>

// Reflected CRC-32/IEEE (poly 0xEDB88320, init/final-xor 0xFFFFFFFF) —
// identical to the photon_rs485 v1 implementation, nibble-table based.
uint32_t photon_crc32(const uint8_t *data, size_t len);

// CRC-8, poly 0x07, init 0x00 — frame v2 header check.
uint8_t photon_crc8(const uint8_t *data, size_t len);

#endif
