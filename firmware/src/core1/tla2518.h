// TI TLA2518 8-channel 12-bit SPI ADC driver. Register map, opcodes and the
// proven access sequences ported from the CircuitPython photon_sensorscan
// module (manual mode, per-slot LED GPO control, OSR oversampling).
#ifndef PHOTON_TLA2518_H
#define PHOTON_TLA2518_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/spi.h"

// Opcodes
#define TLA_OP_REGISTER_READ   0x10
#define TLA_OP_REGISTER_WRITE  0x08
#define TLA_OP_BIT_SET         0x18
#define TLA_OP_BIT_CLEAR       0x20

// Registers
#define TLA_REG_SYSTEM_STATUS  0x00
#define TLA_REG_GENERAL_CFG    0x01
#define TLA_REG_DATA_CFG       0x02
#define TLA_REG_OSR_CONFIG     0x03
#define TLA_REG_OPMODE_CONFIG  0x04
#define TLA_REG_PIN_CFG        0x05
#define TLA_REG_GPIO_CONFIG    0x07
#define TLA_REG_GPIO_DRIVE_CFG 0x09
#define TLA_REG_GPO_VALUE      0x0B
#define TLA_REG_GPI_VALUE      0x0D
#define TLA_REG_SEQUENCE_CFG   0x10
#define TLA_REG_CHANNEL_SEL    0x11

// SYSTEM_STATUS bits
#define TLA_STATUS_BOR         (1u << 0)
#define TLA_STATUS_CRCERR_FUSE (1u << 2)
#define TLA_STATUS_OSR_DONE    (1u << 3)
#define TLA_STATUS_SEQ_STATUS  (1u << 6)

#define TLA_GENERAL_CFG_RST    (1u << 0)

typedef struct {
    spi_inst_t *spi;
    uint8_t cs_pin;
    bool present;      // capability probe result
    uint8_t status;    // last SYSTEM_STATUS refresh
} tla2518_t;

// Bank table (index = bank number, per board_config.h wiring).
extern tla2518_t g_banks[];

// Boot-time (core 0): init SPI buses + CS pins, flush, then probe every bank.
// Returns the number of banks that answered (0 => bridge/endpoint role).
int tla2518_init_and_probe(void);

// Reset + full post-reset configuration of one bank (7-write sequence).
void tla2518_reset_and_configure(int bank);
void tla2518_reset_all(void);

// Hot path (core 1) ------------------------------------------------------

// 3-byte CS-framed register op.
void tla2518_write3(const tla2518_t *b, uint8_t op, uint8_t reg, uint8_t val);
// 2-byte CS-framed transfer; returns the 16-bit MSB-first response word.
uint16_t tla2518_xfer2(const tla2518_t *b);
// Paired 2-byte transfer on two banks (different SPI buses) concurrently.
void tla2518_xfer2_pair(const tla2518_t *ba, const tla2518_t *bb,
                        uint16_t *ra, uint16_t *rb);

// Diagnostics (core 0, not hot path).
uint8_t tla2518_read_reg(const tla2518_t *b, uint8_t reg);
uint8_t tla2518_refresh_status(int bank);

// Decode a raw conversion word per OSR mode: OSR=0 -> 12-bit in [15:4];
// OSR>0 -> full 16-bit accumulated result.
static inline uint16_t tla2518_decode(uint16_t raw, uint8_t osr_mode) {
    return osr_mode == 0 ? (uint16_t)((raw >> 4) & 0x0FFF) : raw;
}

#endif
