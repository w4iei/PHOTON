#include "config/config_store.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/time.h"

#include "core1/events.h"
#include "ipc/rings.h"
#include "util/crc32.h"
#include "util/log.h"

photon_config_t g_config;
bool g_config_from_flash = false;
uint32_t g_config_migrated_from = 0;

static bool core1_running = false;
static int active_sector = -1;  // sector index (0 or 1) holding the loaded copy

static const uint8_t *sector_ptr_at(uint32_t base, int idx) {
    return (const uint8_t *)(XIP_BASE + base +
                             (uint32_t)idx * PHOTON_CONFIG_SECTOR_SIZE);
}

static void fill_defaults(photon_config_t *c);

// *len receives the byte length of the accepted record: sizeof(photon_config_t)
// for the current layout, shorter for a migrated one.
static bool sector_valid_len(uint32_t base, int idx, photon_config_t *out,
                             uint32_t *len_out) {
    photon_config_t c;
    *len_out = 0;
    const uint8_t *raw = sector_ptr_at(base, idx);
    memcpy(&c, raw, sizeof c);
    if (c.magic != PHOTON_CONFIG_MAGIC) {
        return false;
    }
    if (photon_crc32((const uint8_t *)&c, sizeof c - sizeof c.crc) == c.crc) {
        *out = c;
        *len_out = sizeof c;
        return true;
    }
    // Older layout. Fields are only ever appended, so a record written by any
    // earlier build is a prefix of the current one with its CRC in the last
    // four bytes. Find the length whose trailing CRC matches, overlay that
    // prefix on compiled defaults (so the fields it lacks take their default
    // values), and adopt it. No table of historical sizes: every past and
    // future append-only change migrates by construction.
    for (size_t len = PHOTON_CONFIG_MIN_RECORD; len < sizeof c; len++) {
        uint32_t stored;
        memcpy(&stored, raw + len - sizeof stored, sizeof stored);
        if (photon_crc32(raw, len - sizeof stored) == stored) {
            fill_defaults(out);
            memcpy(out, raw, len - sizeof stored);
            *len_out = (uint32_t)len;
            return true;
        }
    }
    return false;
}

static bool sector_valid_at(uint32_t base, int idx, photon_config_t *out) {
    uint32_t len;
    return sector_valid_len(base, idx, out, &len);
}

static bool sector_valid(int idx, photon_config_t *out) {
    return sector_valid_at(PHOTON_CONFIG_FLASH_OFFS, idx, out);
}

static void fill_defaults(photon_config_t *c) {
    memset(c, 0, sizeof *c);
    c->magic = PHOTON_CONFIG_MAGIC;
    c->version = 0;
    c->node_id = 1;
    c->scan_mode = 2;  // PHOTON_SCAN_TWO_PHASE: production mode, see 03-scan-modes.md
    c->midi_low = PHOTON_MIDI_LOW;
    c->midi_high = PHOTON_MIDI_HIGH;
    c->midi_channel = PHOTON_MIDI_CHANNEL;
    c->local_disabled_mask = 1u << 31;  // slot 31 unpopulated by default
    // Every manual is the same 61-key board pair (31 + 30 populated slots),
    // so the historical base config {31, 62, 63} repeats per manual.
    for (uint32_t m = 0; m < PHOTON_MAX_MANUALS; m++) {
        uint32_t base = m * PHOTON_SENSORS_PER_MANUAL;
        static const uint32_t unpop[] = { 31, 62, 63 };
        for (unsigned i = 0; i < 3; i++) {
            uint32_t g = base + unpop[i];
            if (g < PHOTON_GLOBAL_SENSORS) {
                c->global_disabled[g / 8] |= 1u << (g % 8);
            }
        }
    }
    for (int i = 0; i < PHOTON_MAX_SENSORS; i++) {
        c->cal_min[i] = 0xFFFF;
        c->cal_max[i] = 0;
    }
    c->vel_min_ms = PHOTON_VEL_MIN_MS;
    c->vel_max_ms = PHOTON_VEL_MAX_MS;
    c->vel_curve = PHOTON_VEL_CURVE;
    c->vel_out_min = PHOTON_VEL_OUT_MIN;
    c->vel_out_max = PHOTON_VEL_OUT_MAX;
}

static void load_defaults(void) {
    fill_defaults(&g_config);
}

void config_store_init(void) {
    photon_config_t a, b;
    uint32_t la = 0, lb = 0, chosen_len = 0;
    bool va = sector_valid_len(PHOTON_CONFIG_FLASH_OFFS, 0, &a, &la);
    bool vb = sector_valid_len(PHOTON_CONFIG_FLASH_OFFS, 1, &b, &lb);
    if (va && vb) {
        active_sector = (int32_t)(a.version - b.version) >= 0 ? 0 : 1;
        g_config = active_sector == 0 ? a : b;
        chosen_len = active_sector == 0 ? la : lb;
        g_config_from_flash = true;
    } else if (va || vb) {
        active_sector = va ? 0 : 1;
        g_config = va ? a : b;
        chosen_len = va ? la : lb;
        g_config_from_flash = true;
    } else if (sector_valid_len(PHOTON_CONFIG_LEGACY_OFFS, 0, &a, &la) |
               sector_valid_len(PHOTON_CONFIG_LEGACY_OFFS, 1, &b, &lb)) {
        // Flash-size migration: this board was previously flashed by a build
        // that anchored the config to the top of a 16 MB part. Adopt it so
        // node id and calibration survive; the next save writes it to the
        // flash-size-agnostic location and the old copy is simply orphaned.
        bool use_a = la && (!lb || (int32_t)(a.version - b.version) >= 0);
        g_config = use_a ? a : b;
        chosen_len = use_a ? la : lb;
        active_sector = -1;          // force the next save onto the new base
        g_config_from_flash = true;
        log_info("config migrated from the 16 MB location (v%lu); "
                 "it moves on the next save", (unsigned long)g_config.version);
    } else {
        active_sector = -1;
        load_defaults();
        g_config_from_flash = false;
    }
    // Only the record actually adopted counts as a migration: a stale
    // older-layout copy in the other sector is simply superseded.
    if (chosen_len != 0 && chosen_len != sizeof(photon_config_t)) {
        g_config_migrated_from = chosen_len;
        // One-shot: persist in the current layout now (core 1 is not running
        // yet, so no park is needed) so the next boot reads it directly.
        config_store_save();
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
    bool parked = false;
    if (core1_running) {
        parked = cmd_mailbox_park_request(500000);
        if (!parked) {
            log_note("config save: core1 park timed out; writing anyway (SRAM-resident scan)");
        }
    }
    // Sensor role: capture the live calibration before persisting — but only
    // when it has been explicitly frozen ('s'/'x' or remote commit). While
    // learning is active the live min/max is ambient noise; snapshotting it
    // on an unrelated save (setid, rate, localmidi) would persist a
    // degenerate calibration and silently freeze the board dead on reboot.
    // Checked after the park: the park drains the mailbox, so a freeze
    // command pushed just before this save ('s' flow) has taken effect.
    if (core1_running && !g_events.learning) {
        photon_snapshot_t snap;
        snapshot_read(&snap);
        memcpy(g_config.cal_min, snap.min, sizeof g_config.cal_min);
        memcpy(g_config.cal_max, snap.max, sizeof g_config.cal_max);
    }
    g_config.version++;
    g_config.crc = photon_crc32((const uint8_t *)&g_config,
                                sizeof g_config - sizeof g_config.crc);

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
