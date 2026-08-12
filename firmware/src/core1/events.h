// Note-event engine: per-sensor running min/max auto-calibration and the
// two-stage strike/release state machines, ported from the C
// process_scan_events in the CircuitPython fork — with the timebase upgraded
// from 1 ms supervisor ticks to the 1 us hardware timer.
#ifndef PHOTON_EVENTS_H
#define PHOTON_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"
#include "ipc/rings.h"

typedef struct {
    uint16_t value[PHOTON_MAX_SENSORS];
    uint16_t min[PHOTON_MAX_SENSORS];
    uint16_t max[PHOTON_MAX_SENSORS];
    // Calibration mode flag. Learning is ON only during the guided flow
    // ('r' ... play keys one at a time ... 's'): continuous learning
    // during performance lets adjacent-key cross-illumination pollute
    // neighbors' min/max. Frozen (saved) calibration is used otherwise.
    bool     learning;
    uint32_t disabled_mask;      // bit per local sensor
    uint32_t polarity_mask;      // bit set = inverted (pressed = lower value)
    bool     note_on[PHOTON_MAX_SENSORS];
    // strike/release arming state
    bool     strike_pending[PHOTON_MAX_SENSORS];
    uint32_t strike_t_us[PHOTON_MAX_SENSORS];
    bool     release_pending[PHOTON_MAX_SENSORS];
    uint32_t release_t_us[PHOTON_MAX_SENSORS];
    // counters
    uint32_t events_on;
    uint32_t events_off;
} photon_events_t;

extern photon_events_t g_events;

void events_init(uint32_t disabled_mask);

// Load calibration (from config store) for one sensor; freezes auto-learn
// only in the sense that min/max keep expanding from these seeds.
void events_seed_cal(uint8_t idx, uint16_t mn, uint16_t mx);
void events_reset_cal(void);

// Run once per sweep over the fresh readings; pushes note events into
// g_event_ring stamped with now_us.
void events_process(const uint16_t *readings, uint32_t now_us);

#endif
