#include "core1/events.h"

#include <string.h>

photon_events_t g_events;

// Effective range floor scales with the OSR accumulation (170 << 3 = 1360).
#define MIN_EVENT_RANGE_SCALED ((uint32_t)PHOTON_MIN_EVENT_RANGE << PHOTON_OSR_MODE)
#define BOOT_DISABLE_SCALED    ((uint32_t)PHOTON_BOOT_DISABLE_ABOVE << PHOTON_OSR_MODE)

void events_init(uint32_t disabled_mask) {
    memset(&g_events, 0, sizeof g_events);
    g_events.disabled_mask = disabled_mask;
    for (int i = 0; i < PHOTON_MAX_SENSORS; i++) {
        g_events.min[i] = 0xFFFF;
        g_events.max[i] = 0;
    }
}

void events_seed_cal(uint8_t idx, uint16_t mn, uint16_t mx) {
    if (idx >= PHOTON_MAX_SENSORS) {
        return;
    }
    g_events.min[idx] = mn;
    g_events.max[idx] = mx;
}

void events_reset_cal(void) {
    for (int i = 0; i < PHOTON_MAX_SENSORS; i++) {
        g_events.min[i] = 0xFFFF;
        g_events.max[i] = 0;
        g_events.note_on[i] = false;
        g_events.strike_pending[i] = false;
        g_events.release_pending[i] = false;
        g_events.active[i] = false;
    }
}

static inline void clear_sensor_state(photon_events_t *e, int i) {
    e->note_on[i] = false;
    e->strike_pending[i] = false;
    e->release_pending[i] = false;
    e->active[i] = false;
}

void events_process(const uint16_t *readings, uint32_t now_us) {
    photon_events_t *e = &g_events;

    // One-shot boot sanity check (heuristic carried over from the Python
    // runtime): with no calibration yet, a sensor pinned at an implausibly
    // high level is likely stuck/shorted — disable it for this session.
    if (!e->boot_check_done) {
        for (int i = 0; i < PHOTON_ACTIVE_SENSORS; i++) {
            if ((e->disabled_mask >> i) & 1u) {
                continue;
            }
            if (readings[i] > BOOT_DISABLE_SCALED && e->max[i] == 0) {
                e->disabled_mask |= 1u << i;
            }
        }
        e->boot_check_done = true;
    }

    for (int i = 0; i < PHOTON_ACTIVE_SENSORS; i++) {
        if ((e->disabled_mask >> i) & 1u) {
            e->value[i] = 0;
            continue;
        }
        uint16_t v = readings[i];
        e->value[i] = v;
        if (v == 0) {
            continue;  // dead read; never calibrate on it
        }
        if (v < e->min[i]) {
            e->min[i] = v;
        }
        if (v > e->max[i]) {
            e->max[i] = v;
        }
        uint32_t mn = e->min[i], mx = e->max[i];
        uint32_t rng = mx - mn;
        if (rng < MIN_EVENT_RANGE_SCALED) {
            clear_sensor_state(e, i);
            continue;
        }

        // Threshold ladder (integer, percent of range). "Up" direction =
        // key press for normal polarity; inverted sensors mirror around max.
        uint32_t vel_start_pct = PHOTON_STRIKE_PCT > PHOTON_STRIKE_WINDOW_PCT
                                     ? (uint32_t)(PHOTON_STRIKE_PCT - PHOTON_STRIKE_WINDOW_PCT)
                                     : 0;                                     // 30
        uint32_t rel_vel_pct = PHOTON_STRIKE_PCT + PHOTON_VELOCITY_WINDOW_PCT; // 80
        if (rel_vel_pct > 100) {
            rel_vel_pct = 100;
        }
        uint32_t rel_pct = PHOTON_RELEASE_PCT < PHOTON_STRIKE_PCT
                               ? PHOTON_RELEASE_PCT
                               : PHOTON_STRIKE_PCT;                            // 40

        uint32_t press;  // how far into the range the key currently is, 0..rng
        if ((e->polarity_mask >> i) & 1u) {
            press = mx - v;
        } else {
            press = v - mn;
        }
        uint32_t vel_start_thr = rng * vel_start_pct / 100;
        uint32_t strike_thr = rng * PHOTON_STRIKE_PCT / 100;
        uint32_t rel_vel_thr = rng * rel_vel_pct / 100;
        uint32_t rel_thr = rng * rel_pct / 100;
        uint32_t act_thr = rng * PHOTON_ACTIVATION_PCT / 100;

        e->active[i] = press >= act_thr;

        if (!e->note_on[i]) {
            // Strike: arm on crossing the velocity-start threshold, fire ON
            // at the strike threshold; dt = time between the two crossings.
            if (!e->strike_pending[i]) {
                if (press >= vel_start_thr && press < strike_thr) {
                    e->strike_pending[i] = true;
                    e->strike_t_us[i] = now_us;
                } else if (press >= strike_thr) {
                    // Jumped both thresholds inside one sweep: fastest
                    // measurable strike — dt of one sweep period is stamped
                    // by arming and firing at the same instant.
                    e->strike_pending[i] = true;
                    e->strike_t_us[i] = now_us;
                }
            }
            if (e->strike_pending[i]) {
                if (press >= strike_thr) {
                    uint32_t dt = now_us - e->strike_t_us[i];
                    e->note_on[i] = true;
                    e->strike_pending[i] = false;
                    e->release_pending[i] = false;
                    e->events_on++;
                    event_ring_push((uint8_t)i, 1, dt, now_us);
                } else if (press < vel_start_thr) {
                    e->strike_pending[i] = false;  // disarm: retreated
                }
            }
        } else {
            // Release: symmetric, coming down through the release-velocity
            // threshold then the release threshold.
            if (!e->release_pending[i]) {
                if (press <= rel_vel_thr) {
                    e->release_pending[i] = true;
                    e->release_t_us[i] = now_us;
                }
            }
            if (e->release_pending[i]) {
                if (press <= rel_thr) {
                    uint32_t dt = now_us - e->release_t_us[i];
                    e->note_on[i] = false;
                    e->release_pending[i] = false;
                    e->events_off++;
                    event_ring_push((uint8_t)i, 0, dt, now_us);
                } else if (press > rel_vel_thr) {
                    e->release_pending[i] = false;  // disarm: went back up
                }
            }
        }
    }
}
