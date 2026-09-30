// QMI setup follows the sequence CircuitPython (ports/raspberrypi) and the
// SparkFun RP2350 PSRAM helper use for the APS6404L: exit QPI, read the ID
// in SPI mode, reset + enter QPI, then program window 1 for 0xEB quad reads
// (6 wait cycles) and 0x38 quad writes. Runs from RAM with interrupts off
// because it briefly takes the QMI (and so flash XIP) into direct mode.
#include "hw/psram.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"

#include "board_config.h"

static size_t size_bytes;
static int cs_pin = -1;
static uint8_t kgd_byte;

static void __no_inline_not_in_flash_func(direct_wait)(void) {
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
    }
}

// Read MF ID / KGD / EID in SPI mode. Returns kgd; *eid receives the size byte.
static uint8_t __no_inline_not_in_flash_func(read_id)(uint8_t *eid) {
    qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    direct_wait();

    // Leave QPI in case a previous boot put the chip there (0xF5 sent quad).
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
                        QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB | 0xF5;
    direct_wait();
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    // 0x9F, 3 address bytes, then MFID, KGD, EID.
    uint8_t kgd = 0;
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    for (int i = 0; i < 7; i++) {
        qmi_hw->direct_tx = i == 0 ? 0x9F : 0xFF;
        while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS)) {
        }
        direct_wait();
        uint8_t v = (uint8_t)qmi_hw->direct_rx;
        if (i == 5) {
            kgd = v;
        } else if (i == 6) {
            *eid = v;
        }
    }
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);
    return kgd;
}

static void __no_inline_not_in_flash_func(enter_qpi_and_map)(void) {
    qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    direct_wait();
    // Reset enable, reset, QPI on. Immediates, not a table: flash (.rodata)
    // is unreachable while the QMI is in direct mode.
    for (int i = 0; i < 3; i++) {
        qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        qmi_hw->direct_tx = i == 0 ? 0x66 : i == 1 ? 0x99 : 0x35;
        direct_wait();
        qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
        for (volatile int j = 0; j < 20; j++) {
        }
        (void)qmi_hw->direct_rx;
    }
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    // Timing from clk_sys: SCK <= 133 MHz; CS low <= 8 us (MAX_SELECT is in
    // 64-cycle units); CS high >= 18 ns between bursts; 1 KB page breaks.
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t div = (sys_hz + 133000000 - 1) / 133000000;
    if (div == 1 && sys_hz > 100000000) {
        div = 2;
    }
    uint32_t rxdelay = div + (sys_hz / div > 100000000 ? 1 : 0);
    uint64_t period_fs = 1000000000000000ull / sys_hz;
    uint32_t max_select = (uint32_t)((125ull * 1000000ull) / period_fs);  // 8 us / 64
    uint32_t min_deselect = (uint32_t)((18ull * 1000000ull + period_fs - 1) / period_fs) - (div + 1) / 2;
    qmi_hw->m[1].timing = 1 << QMI_M1_TIMING_COOLDOWN_LSB |
                          QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
                          max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
                          min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
                          rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
                          div << QMI_M1_TIMING_CLKDIV_LSB;
    qmi_hw->m[1].rfmt = QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB |
                        QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_RFMT_ADDR_WIDTH_LSB |
                        QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB |
                        QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_RFMT_DUMMY_WIDTH_LSB |
                        QMI_M1_RFMT_DATA_WIDTH_VALUE_Q << QMI_M1_RFMT_DATA_WIDTH_LSB |
                        QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB |
                        6 << QMI_M1_RFMT_DUMMY_LEN_LSB;
    qmi_hw->m[1].rcmd = 0xEB;
    qmi_hw->m[1].wfmt = QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB |
                        QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_WFMT_ADDR_WIDTH_LSB |
                        QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB |
                        QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_WFMT_DUMMY_WIDTH_LSB |
                        QMI_M1_WFMT_DATA_WIDTH_VALUE_Q << QMI_M1_WFMT_DATA_WIDTH_LSB |
                        QMI_M1_WFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB;
    qmi_hw->m[1].wcmd = 0x38;
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
}

size_t psram_init(void) {
    static const uint8_t candidates[] = { PHOTON_PSRAM_CS };
    for (size_t c = 0; c < sizeof candidates; c++) {
        uint pin = candidates[c];
        gpio_set_function(pin, GPIO_FUNC_XIP_CS1);
        uint32_t irq = save_and_disable_interrupts();
        uint8_t eid = 0;
        uint8_t kgd = read_id(&eid);
        if (kgd == 0x5D) {
            enter_qpi_and_map();
            restore_interrupts(irq);
            kgd_byte = kgd;
            cs_pin = (int)pin;
            // EID bits 7:5 give the density: 0 = 2 MB, 1 = 4 MB, 2 = 8 MB.
            uint8_t density = eid >> 5;
            size_bytes = (density == 0 ? 2u : density == 1 ? 4u : 8u) * 1024u * 1024u;
            if (psram_test(64 * 1024) != 0) {
                size_bytes = 0;
            }
            return size_bytes;
        }
        restore_interrupts(irq);
        gpio_init(pin);  // not here: give the pin back as a plain input
    }
    return 0;
}

size_t psram_size(void) { return size_bytes; }
int psram_cs_pin(void) { return cs_pin; }
uint8_t psram_kgd(void) { return kgd_byte; }

uint32_t psram_test(size_t bytes) {
    if (cs_pin < 0) {
        return 1;
    }
    // Uncached alias so the test exercises the chip, not the XIP cache.
    volatile uint32_t *p = (volatile uint32_t *)PHOTON_PSRAM_BASE_NC;
    size_t words = bytes / 4;
    for (size_t i = 0; i < words; i++) {
        p[i] = (uint32_t)i * 2654435761u ^ 0xA5A5A5A5u;
    }
    uint32_t bad = 0;
    for (size_t i = 0; i < words; i++) {
        if (p[i] != ((uint32_t)i * 2654435761u ^ 0xA5A5A5A5u)) {
            bad++;
        }
    }
    return bad;
}
