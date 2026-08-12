#include "config/config_store.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/time.h"

#include "ipc/rings.h"
#include "util/crc32.h"
#include "util/log.h"

photon_config_t g_config;
bool g_config_from_flash = false;

static bool core1_running = false;
static int active_sector = -1;  // sector index (0 or 1) holding the loaded copy

static const uint8_t *sector_ptr(int idx) {
    return (const uint8_t *)(XIP_BASE + PHOTON_CONFIG_FLASH_OFFS +
                             (uint32_t)idx * PHOTON_CONFIG_SECTOR_SIZE);
}

static bool sector_valid(int idx, photon_config_t *out) {
    photon_config_t c;
    memcpy(&c, sector_ptr(idx), sizeof c);
    if (c.magic != PHOTON_CONFIG_MAGIC) {
        return false;
    }
    uint32_t crc = photon_crc32((const uint8_t *)&c, sizeof c - sizeof c.crc);
    if (crc != c.crc) {
        return false;
    }
    *out = c;
    return true;
}

static void load_defaults(void) {
    memset(&g_config, 0, sizeof g_config);
    g_config.magic = PHOTON_CONFIG_MAGIC;
    g_config.version = 0;
    g_config.node_id = 1;
    g_config.scan_mode = 1;  // PHOTON_SCAN_PARALLEL
    g_config.midi_low = PHOTON_MIDI_LOW;
    g_config.midi_high = PHOTON_MIDI_HIGH;
    g_config.midi_channel = PHOTON_MIDI_CHANNEL;
    g_config.local_disabled_mask = 1u << 31;  // slot 31 unpopulated by default
    // Historical base config: global sensors 31, 62, 63 disabled.
    g_config.global_disabled[31 / 8] |= 1u << (31 % 8);
    g_config.global_disabled[62 / 8] |= 1u << (62 % 8);
    g_config.global_disabled[63 / 8] |= 1u << (63 % 8);
    for (int i = 0; i < PHOTON_MAX_SENSORS; i++) {
        g_config.cal_min[i] = 0xFFFF;
        g_config.cal_max[i] = 0;
    }
    g_config.vel_min_ms = PHOTON_VEL_MIN_MS;
    g_config.vel_max_ms = PHOTON_VEL_MAX_MS;
    g_config.vel_curve = PHOTON_VEL_CURVE;
}

void config_store_init(void) {
    photon_config_t a, b;
    bool va = sector_valid(0, &a);
    bool vb = sector_valid(1, &b);
    if (va && vb) {
        active_sector = (int32_t)(a.version - b.version) >= 0 ? 0 : 1;
        g_config = active_sector == 0 ? a : b;
        g_config_from_flash = true;
    } else if (va || vb) {
        active_sector = va ? 0 : 1;
        g_config = va ? a : b;
        g_config_from_flash = true;
    } else {
        active_sector = -1;
        load_defaults();
        g_config_from_flash = false;
    }
}

void config_store_set_core1_running(bool running) {
    core1_running = running;
}

static bool write_sector(int idx, const photon_config_t *cfg) {
    // Stage a full page-aligned image.
    static uint8_t page_buf[((sizeof(photon_config_t) + FLASH_PAGE_SIZE - 1) /
                             FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE];
    memset(page_buf, 0xFF, sizeof page_buf);
    memcpy(page_buf, cfg, sizeof *cfg);

    uint32_t offs = PHOTON_CONFIG_FLASH_OFFS + (uint32_t)idx * PHOTON_CONFIG_SECTOR_SIZE;
    uint32_t save = save_and_disable_interrupts();
    flash_range_erase(offs, PHOTON_CONFIG_SECTOR_SIZE);
    flash_range_program(offs, page_buf, sizeof page_buf);
    restore_interrupts(save);

    photon_config_t verify;
    return sector_valid(idx, &verify) && verify.version == cfg->version;
}

bool config_store_save(void) {
    // Sensor role: capture the live learned calibration before persisting.
    if (core1_running) {
        photon_snapshot_t snap;
        snapshot_read(&snap);
        memcpy(g_config.cal_min, snap.min, sizeof g_config.cal_min);
        memcpy(g_config.cal_max, snap.max, sizeof g_config.cal_max);
    }
    g_config.version++;
    g_config.crc = photon_crc32((const uint8_t *)&g_config,
                                sizeof g_config - sizeof g_config.crc);

    bool parked = false;
    if (core1_running) {
        parked = cmd_mailbox_park_request(500000);
        if (!parked) {
            log_note("config save: core1 park timed out; writing anyway (SRAM-resident scan)");
        }
    }
    int target = active_sector == 0 ? 1 : 0;
    bool ok = write_sector(target, &g_config);
    if (parked) {
        cmd_mailbox_park_release();
    }
    if (ok) {
        active_sector = target;
        g_config_from_flash = true;
        log_info("config v%lu saved to sector %d", (unsigned long)g_config.version, target);
    } else {
        log_note("config save FAILED");
    }
    return ok;
}

int config_store_flashtest(int cycles) {
    // Deliberately no core-1 park: this is the M1 proof that flash erase/
    // program cannot perturb the SRAM-resident scan loop.
    int target = active_sector == 0 ? 1 : 0;
    if (active_sector < 0) {
        target = 1;  // never clobber sector 0 defaults-candidate
    }
    uint32_t offs = PHOTON_CONFIG_FLASH_OFFS + (uint32_t)target * PHOTON_CONFIG_SECTOR_SIZE;
    static uint8_t junk[FLASH_PAGE_SIZE];
    for (int c = 0; c < cycles; c++) {
        memset(junk, (uint8_t)c, sizeof junk);
        uint32_t save = save_and_disable_interrupts();
        flash_range_erase(offs, PHOTON_CONFIG_SECTOR_SIZE);
        flash_range_program(offs, junk, sizeof junk);
        restore_interrupts(save);
        sleep_ms(1);
    }
    return cycles;
}
