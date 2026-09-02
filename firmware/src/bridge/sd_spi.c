// SD card, SPI mode (SD Physical Layer Simplified Spec part 1, chapter 7):
// the classic CMD0 -> CMD8 -> ACMD41 -> CMD58 bring-up at <= 400 kHz, then
// single/multi block reads and writes at PHOTON_SD_BAUD_HZ. No CRC on data
// (SPI mode allows it off); the FatFs layer above notices bad sectors.
#include "bridge/sd_spi.h"

#include <string.h>

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/time.h"

#include "board_config.h"
#include "ff.h"
#include "diskio.h"

enum { CT_NONE = 0, CT_MMC = 0x01, CT_SD1 = 0x02, CT_SD2 = 0x04, CT_BLOCK = 0x08 };

#define CMD0   0            // GO_IDLE_STATE
#define CMD1   1            // SEND_OP_COND (MMC)
#define CMD8   8            // SEND_IF_COND
#define CMD9   9            // SEND_CSD
#define CMD12  12           // STOP_TRANSMISSION
#define CMD16  16           // SET_BLOCKLEN
#define CMD17  17           // READ_SINGLE_BLOCK
#define CMD18  18           // READ_MULTIPLE_BLOCK
#define CMD24  24           // WRITE_BLOCK
#define CMD25  25           // WRITE_MULTIPLE_BLOCK
#define CMD55  55           // APP_CMD
#define CMD58  58           // READ_OCR
#define ACMD23 (0x80 | 23)  // SET_WR_BLK_ERASE_COUNT
#define ACMD41 (0x80 | 41)  // SD_SEND_OP_COND

static uint8_t card_type;
static uint32_t sector_count;

static inline void cs_low(void) { gpio_put(PHOTON_SD_CS, 0); }
static inline void cs_high(void) { gpio_put(PHOTON_SD_CS, 1); }

static uint8_t xfer(uint8_t out) {
    uint8_t in;
    spi_write_read_blocking(PHOTON_SD_SPI, &out, &in, 1);
    return in;
}

// Card signals busy by holding DO low.
static bool wait_ready(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    do {
        if (xfer(0xFF) == 0xFF) {
            return true;
        }
    } while (!time_reached(deadline));
    return false;
}

static void deselect(void) {
    cs_high();
    xfer(0xFF);  // the card needs 8 clocks after CS rises to release DO
}

static bool select(void) {
    cs_low();
    xfer(0xFF);
    if (wait_ready(500)) {
        return true;
    }
    deselect();
    return false;
}

// Returns R1 (bit 7 clear), or 0xFF on timeout. ACMDs (0x80 | n) send the
// CMD55 prefix themselves.
static uint8_t send_cmd(uint8_t cmd, uint32_t arg) {
    if (cmd & 0x80) {
        cmd &= 0x7F;
        uint8_t r = send_cmd(CMD55, 0);
        if (r > 1) {
            return r;
        }
    }
    if (cmd != CMD12) {
        deselect();
        if (!select()) {
            return 0xFF;
        }
    }
    uint8_t frame[6] = {
        (uint8_t)(0x40 | cmd),
        (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg,
        0x01,  // dummy CRC + stop bit
    };
    if (cmd == CMD0) {
        frame[5] = 0x95;  // valid CRC7 for CMD0(0)
    } else if (cmd == CMD8) {
        frame[5] = 0x87;  // valid CRC7 for CMD8(0x1AA)
    }
    spi_write_blocking(PHOTON_SD_SPI, frame, sizeof frame);
    if (cmd == CMD12) {
        xfer(0xFF);  // discard the stuff byte
    }
    uint8_t r;
    int n = 10;
    do {
        r = xfer(0xFF);
    } while ((r & 0x80) && --n);
    return r;
}

// Poll a command until it answers 0 (card ready) or the timeout passes.
static bool wait_cmd_zero(uint8_t cmd, uint32_t arg, uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    do {
        if (send_cmd(cmd, arg) == 0) {
            return true;
        }
    } while (!time_reached(deadline));
    return false;
}

static bool rcvr_datablock(uint8_t *buf, uint32_t len) {
    absolute_time_t deadline = make_timeout_time_ms(200);
    uint8_t token;
    do {
        token = xfer(0xFF);
    } while (token == 0xFF && !time_reached(deadline));
    if (token != 0xFE) {
        return false;
    }
    spi_read_blocking(PHOTON_SD_SPI, 0xFF, buf, len);
    xfer(0xFF);  // CRC, ignored
    xfer(0xFF);
    return true;
}

// token: 0xFE single, 0xFC multi-block data, 0xFD multi-block stop.
static bool xmit_datablock(const uint8_t *buf, uint8_t token) {
    if (!wait_ready(500)) {
        return false;
    }
    xfer(token);
    if (token != 0xFD) {
        spi_write_blocking(PHOTON_SD_SPI, buf, 512);
        xfer(0xFF);  // dummy CRC
        xfer(0xFF);
        uint8_t resp = xfer(0xFF);
        if ((resp & 0x1F) != 0x05) {  // data accepted
            return false;
        }
    }
    return true;
}

static bool read_csd_sector_count(void) {
    uint8_t csd[16];
    if (send_cmd(CMD9, 0) != 0 || !rcvr_datablock(csd, sizeof csd)) {
        return false;
    }
    if ((csd[0] >> 6) == 1) {
        // CSD v2 (SDHC/SDXC): capacity = (C_SIZE + 1) * 512 KB
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
        sector_count = (c_size + 1) * 1024u;
    } else {
        // CSD v1 (SDSC/MMC)
        uint8_t n = (uint8_t)((csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2);
        uint32_t c_size = (uint32_t)(csd[8] >> 6) + ((uint32_t)csd[7] << 2) +
                          ((uint32_t)(csd[6] & 3) << 10) + 1;
        sector_count = c_size << (n - 9);
    }
    return true;
}

bool sd_spi_init(void) {
    card_type = CT_NONE;
    sector_count = 0;

    gpio_init(PHOTON_SD_CS);
    gpio_set_dir(PHOTON_SD_CS, GPIO_OUT);
    cs_high();
    // The sensor probe muxed GPIO8 as SPI1 RX; hand that input back to SIO so
    // it cannot contend with the card's DO on GPIO12.
    gpio_init(PHOTON_SPI_A_MISO);

    spi_init(PHOTON_SD_SPI, PHOTON_SD_BAUD_INIT_HZ);
    spi_set_format(PHOTON_SD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PHOTON_SD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PHOTON_SD_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PHOTON_SD_MISO, GPIO_FUNC_SPI);
    gpio_pull_up(PHOTON_SD_MISO);

    // >= 74 clocks with CS high puts the card into SPI mode on CMD0.
    for (int i = 0; i < 10; i++) {
        xfer(0xFF);
    }

    uint8_t type = CT_NONE;
    if (send_cmd(CMD0, 0) == 1) {
        if (send_cmd(CMD8, 0x1AA) == 1) {
            // SD v2: check the echoed voltage/pattern, then ACMD41 with HCS.
            uint8_t r7[4];
            for (int i = 0; i < 4; i++) {
                r7[i] = xfer(0xFF);
            }
            if (r7[2] == 0x01 && r7[3] == 0xAA &&
                wait_cmd_zero(ACMD41, 1u << 30, 1000) && send_cmd(CMD58, 0) == 0) {
                uint8_t ocr[4];
                for (int i = 0; i < 4; i++) {
                    ocr[i] = xfer(0xFF);
                }
                type = (ocr[0] & 0x40) ? (CT_SD2 | CT_BLOCK) : CT_SD2;
            }
        } else {
            // SD v1 or MMC v3: byte addressing, 512-byte blocks.
            uint8_t cmd;
            if (send_cmd(ACMD41, 0) <= 1) {
                type = CT_SD1;
                cmd = ACMD41;
            } else {
                type = CT_MMC;
                cmd = CMD1;
            }
            if (!wait_cmd_zero(cmd, 0, 1000) || send_cmd(CMD16, 512) != 0) {
                type = CT_NONE;
            }
        }
    }
    if (type != CT_NONE) {
        card_type = type;  // read_csd needs send_cmd, which is type-agnostic
        if (!read_csd_sector_count()) {
            type = CT_NONE;
        }
    }
    deselect();
    card_type = type;
    if (type == CT_NONE) {
        return false;
    }
    spi_set_baudrate(PHOTON_SD_SPI, PHOTON_SD_BAUD_HZ);
    return true;
}

bool sd_spi_ready(void) { return card_type != CT_NONE; }
uint32_t sd_spi_sector_count(void) { return sector_count; }

bool sd_spi_read(uint32_t lba, uint8_t *buf, uint32_t count) {
    if (card_type == CT_NONE || count == 0) {
        return false;
    }
    if (!(card_type & CT_BLOCK)) {
        lba *= 512;
    }
    bool ok = false;
    if (count == 1) {
        ok = send_cmd(CMD17, lba) == 0 && rcvr_datablock(buf, 512);
    } else if (send_cmd(CMD18, lba) == 0) {
        ok = true;
        for (; count; count--, buf += 512) {
            if (!rcvr_datablock(buf, 512)) {
                ok = false;
                break;
            }
        }
        send_cmd(CMD12, 0);
    }
    deselect();
    if (!ok) {
        card_type = CT_NONE;
    }
    return ok;
}

bool sd_spi_write(uint32_t lba, const uint8_t *buf, uint32_t count) {
    if (card_type == CT_NONE || count == 0) {
        return false;
    }
    if (!(card_type & CT_BLOCK)) {
        lba *= 512;
    }
    bool ok = false;
    if (count == 1) {
        ok = send_cmd(CMD24, lba) == 0 && xmit_datablock(buf, 0xFE);
    } else {
        if (card_type & (CT_SD1 | CT_SD2)) {
            send_cmd(ACMD23, count);  // pre-erase hint, optional
        }
        if (send_cmd(CMD25, lba) == 0) {
            ok = true;
            for (; count; count--, buf += 512) {
                if (!xmit_datablock(buf, 0xFC)) {
                    ok = false;
                    break;
                }
            }
            if (!xmit_datablock(NULL, 0xFD)) {
                ok = false;
            }
        }
    }
    if (ok) {
        ok = wait_ready(500);
    }
    deselect();
    if (!ok) {
        card_type = CT_NONE;
    }
    return ok;
}

bool sd_spi_sync(void) {
    if (card_type == CT_NONE) {
        return false;
    }
    bool ok = select();
    deselect();
    return ok;
}

// ---------------------------------------------------------------------------
// FatFs diskio, physical drive 0 = the card
// ---------------------------------------------------------------------------

DSTATUS disk_status(BYTE pdrv) {
    return (pdrv == 0 && sd_spi_ready()) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    return (pdrv == 0 && sd_spi_init()) ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0) {
        return RES_PARERR;
    }
    if (!sd_spi_ready()) {
        return RES_NOTRDY;
    }
    return sd_spi_read((uint32_t)sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0) {
        return RES_PARERR;
    }
    if (!sd_spi_ready()) {
        return RES_NOTRDY;
    }
    return sd_spi_write((uint32_t)sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0) {
        return RES_PARERR;
    }
    if (!sd_spi_ready()) {
        return RES_NOTRDY;
    }
    switch (cmd) {
    case CTRL_SYNC:
        return sd_spi_sync() ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = sd_spi_sector_count();
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
