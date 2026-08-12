// Persistent per-node configuration: two 4 KB A/B sectors at the top of
// flash, version counter + CRC32, newest valid copy wins. Replaces the
// CIRCUITPY JSON file, /sensor_node_id marker and NVM flag byte.
#ifndef PHOTON_CONFIG_STORE_H
#define PHOTON_CONFIG_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

#define PHOTON_CONFIG_MAGIC 0x4E544850u  // "PHTN"

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;         // monotonically bumped on each save
    uint8_t node_id;          // 1..6 (ignored in bridge role)
    uint8_t scan_mode;        // photon_scan_mode_t startup default
    uint8_t midi_low;
    uint8_t midi_high;
    uint8_t midi_channel;
    // Carved from former padding so pre-existing saved configs stay valid
    // (old sectors read 0 here): 0 = default rate, 0xFFFF = unthrottled.
    uint16_t scan_rate_hz;
    uint8_t _pad1;
    uint32_t local_disabled_mask;
    uint8_t global_disabled[(PHOTON_GLOBAL_SENSORS + 7) / 8];
    uint16_t cal_min[PHOTON_MAX_SENSORS];
    uint16_t cal_max[PHOTON_MAX_SENSORS];
    float vel_min_ms;
    float vel_max_ms;
    float vel_curve;
    uint32_t crc;             // CRC32 over all preceding bytes
} photon_config_t;

extern photon_config_t g_config;
extern bool g_config_from_flash;  // false = compiled defaults ("uncalibrated")

// Load newest valid sector or defaults. Core 0, before core-1 launch.
void config_store_init(void);

// Persist current state (sensor role: live min/max are snapshotted first).
// Parks core 1 during the erase/program when the scan core is running.
bool config_store_save(void);

// Set by main once core 1 is launched, so save() knows to park.
void config_store_set_core1_running(bool running);

// M1 bench hook: hammer the inactive sector WITHOUT parking core 1 to prove
// flash activity cannot stall the SRAM-resident scan loop. Returns cycles done.
int config_store_flashtest(int cycles);

#endif
