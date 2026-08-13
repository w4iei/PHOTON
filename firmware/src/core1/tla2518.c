#include "core1/tla2518.h"

#include <string.h>

#include "hardware/gpio.h"
#include "pico/time.h"

#include "board_config.h"
#include "util/log.h"

tla2518_t g_banks[PHOTON_BANK_COUNT];

static const uint8_t bank_cs_pins[PHOTON_BANK_COUNT] = PHOTON_BANK_CS_PINS;

// --------------------------------------------------------------------------
// SPI access: single-bank ops use the SDK's interleaved blocking transfers
// (all frames are 2-3 bytes, far below any sensible DMA threshold). Only the
// dual-bus paired transfer is hand-rolled — the SDK has no two-peripheral
// concurrent primitive.
// --------------------------------------------------------------------------

static inline void cs_low(const tla2518_t *b) { gpio_put(b->cs_pin, 0); }
static inline void cs_high(const tla2518_t *b) { gpio_put(b->cs_pin, 1); }

void tla2518_write3(const tla2518_t *b, uint8_t op, uint8_t reg, uint8_t val) {
    uint8_t frame[3] = { op, reg, val };
    cs_low(b);
    spi_write_blocking(b->spi, frame, 3);
    cs_high(b);
}

uint16_t tla2518_xfer2(const tla2518_t *b) {
    uint8_t rx[2];
    cs_low(b);
    spi_read_blocking(b->spi, 0x00, rx, 2);
    cs_high(b);
    return (uint16_t)((rx[0] << 8) | rx[1]);
}

void tla2518_xfer2_pair(const tla2518_t *ba, const tla2518_t *bb,
                        uint16_t *ra, uint16_t *rb) {
    // Both banks sit on different SPI peripherals: queue both frames, then
    // drain both — the two buses clock out concurrently.
    spi_hw_t *ha = spi_get_hw(ba->spi);
    spi_hw_t *hb = spi_get_hw(bb->spi);
    uint8_t rxa[2], rxb[2];
    cs_low(ba);
    cs_low(bb);
    ha->dr = 0; ha->dr = 0;   // 8-deep TX FIFO: no TNF check needed for 2 bytes
    hb->dr = 0; hb->dr = 0;
    for (int i = 0; i < 2; i++) {
        while (!(ha->sr & SPI_SSPSR_RNE_BITS)) { tight_loop_contents(); }
        rxa[i] = (uint8_t)ha->dr;
    }
    for (int i = 0; i < 2; i++) {
        while (!(hb->sr & SPI_SSPSR_RNE_BITS)) { tight_loop_contents(); }
        rxb[i] = (uint8_t)hb->dr;
    }
    cs_high(ba);
    cs_high(bb);
    *ra = (uint16_t)((rxa[0] << 8) | rxa[1]);
    *rb = (uint16_t)((rxb[0] << 8) | rxb[1]);
}

uint8_t tla2518_read_reg(const tla2518_t *b, uint8_t reg) {
    tla2518_write3(b, TLA_OP_REGISTER_READ, reg, 0x00);
    uint16_t w = tla2518_xfer2(b);
    return (uint8_t)(w >> 8);
}

// --------------------------------------------------------------------------
// Init / probe / configure
// --------------------------------------------------------------------------

static void bus_setup(spi_inst_t *spi, uint sclk, uint mosi, uint miso) {
    spi_init(spi, PHOTON_SPI_BAUD_HZ);
    spi_set_format(spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(sclk, GPIO_FUNC_SPI);
    gpio_set_function(mosi, GPIO_FUNC_SPI);
    gpio_set_function(miso, GPIO_FUNC_SPI);
}

static void bus_flush(spi_inst_t *spi) {
    // Proven bring-up ritual from photon_sensorscan: with all CS idle high,
    // clock 16 zero bytes at 1 MHz to clear any half-started transaction.
    spi_set_baudrate(spi, 1000000);
    sleep_us(PHOTON_TLA_BUS_FLUSH_US);
    uint8_t zeros[16] = { 0 };
    spi_write_blocking(spi, zeros, sizeof zeros);
    spi_set_baudrate(spi, PHOTON_SPI_BAUD_HZ);
}

void tla2518_reset_and_configure(int bank) {
    tla2518_t *b = &g_banks[bank];
    tla2518_write3(b, TLA_OP_REGISTER_WRITE, TLA_REG_GENERAL_CFG, TLA_GENERAL_CFG_RST);
    sleep_us(PHOTON_TLA_RESET_WAIT_US);
    // Post-reset configuration (verbatim sequence from photon_sensorscan):
    tla2518_write3(b, TLA_OP_BIT_CLEAR, TLA_REG_SEQUENCE_CFG, 0x03);      // manual mode
    tla2518_write3(b, TLA_OP_BIT_SET, TLA_REG_PIN_CFG, PHOTON_EMITTER_MASK_ALL);
    tla2518_write3(b, TLA_OP_BIT_SET, TLA_REG_GPIO_CONFIG, PHOTON_EMITTER_MASK_ALL);
    tla2518_write3(b, TLA_OP_BIT_SET, TLA_REG_GPIO_DRIVE_CFG, PHOTON_EMITTER_MASK_ALL);
    tla2518_write3(b, TLA_OP_BIT_CLEAR, TLA_REG_GPO_VALUE, PHOTON_EMITTER_MASK_ALL);
    tla2518_write3(b, TLA_OP_REGISTER_WRITE, TLA_REG_DATA_CFG, 0x00);
    tla2518_write3(b, TLA_OP_REGISTER_WRITE, TLA_REG_OSR_CONFIG, PHOTON_OSR_MODE & 0x07);
}

void tla2518_reset_all(void) {
    for (int i = 0; i < PHOTON_BANK_COUNT; i++) {
        if (g_banks[i].present) {
            tla2518_reset_and_configure(i);
        }
    }
}

static void bank_cs_init(int i) {
    gpio_init(g_banks[i].cs_pin);
    gpio_put(g_banks[i].cs_pin, 1);
    gpio_set_dir(g_banks[i].cs_pin, GPIO_OUT);
}

static bool bank_probe(int i) {
    // Probe: program OSR_CONFIG and read it back. A floating/absent MISO
    // yields 0x00 or 0xFF, never the written value.
    tla2518_t *b = &g_banks[i];
    tla2518_write3(b, TLA_OP_REGISTER_WRITE, TLA_REG_OSR_CONFIG, PHOTON_OSR_MODE & 0x07);
    uint8_t readback = tla2518_read_reg(b, TLA_REG_OSR_CONFIG);
    b->present = (readback == (PHOTON_OSR_MODE & 0x07));
    return b->present;
}

int tla2518_init_and_probe(void) {
    // The probe runs BEFORE the board's identity is known, and two sensor
    // CS pins are hazardous on the main controller board: GPIO1 is its
    // RS-485 DE (driving it high would enable the transmitter) and GPIO5
    // is its UART RX (push-pull against the transceiver's driven R output).
    //
    // Additional subtlety (found on hardware): GPIO1/GPIO5 are the chip
    // selects of banks 4 and 6 on bus B, and the RP2350's power-on pull-
    // downs hold those chips SELECTED while the pins are untouched — so
    // probing any other bus-B bank first gets a garbled readback from
    // MISO contention. Therefore: stage 1 probes bus A only (banks 0-3,
    // CS 21/20/19/15 — no shared bus with the hazardous pins, benign on
    // the main board). Any response proves sensor-board identity; only
    // then are GPIO1/GPIO5 raised and all of bus B probed cleanly.
    static const uint8_t bus_a_banks[] = { 0, 1, 2, 3 };  // CS 21,20,19,15
    static const uint8_t bus_b_banks[] = { 4, 5, 6, 7 };  // CS 1,7,5,6

    memset(g_banks, 0, sizeof g_banks);
    for (int i = 0; i < PHOTON_BANK_COUNT; i++) {
        g_banks[i].spi = PHOTON_BANK_ON_BUS_B(i) ? PHOTON_SPI_B : PHOTON_SPI_A;
        g_banks[i].cs_pin = bank_cs_pins[i];
    }
    for (size_t k = 0; k < sizeof bus_a_banks; k++) {
        bank_cs_init(bus_a_banks[k]);
    }
    bus_setup(PHOTON_SPI_A, PHOTON_SPI_A_SCLK, PHOTON_SPI_A_MOSI, PHOTON_SPI_A_MISO);
    bus_setup(PHOTON_SPI_B, PHOTON_SPI_B_SCLK, PHOTON_SPI_B_MOSI, PHOTON_SPI_B_MISO);
    sleep_ms(50);  // CS-idle settle before first transaction (proven bring-up value)
    bus_flush(PHOTON_SPI_A);
    bus_flush(PHOTON_SPI_B);

    int found = 0;
    for (size_t k = 0; k < sizeof bus_a_banks; k++) {
        if (bank_probe(bus_a_banks[k])) {
            found++;
        }
    }
    if (found > 0) {
        // Confirmed sensor board: raise every bus-B CS (deselecting all
        // four chips), settle, flush, then probe them one at a time.
        for (size_t k = 0; k < sizeof bus_b_banks; k++) {
            bank_cs_init(bus_b_banks[k]);
        }
        sleep_ms(1);
        bus_flush(PHOTON_SPI_B);
        for (size_t k = 0; k < sizeof bus_b_banks; k++) {
            if (bank_probe(bus_b_banks[k])) {
                found++;
            }
        }
    }
    // A board whose entire bus A is dead but bus B is alive would misprobe
    // as a bridge here; that residual ambiguity needs a board-ID strap on
    // the next hardware revision (bus B cannot be probed without first
    // driving pins that are unsafe on the main controller board).
    return found;
}

