#include "core1/calcap.h"

#include <string.h>

#include "hardware/sync.h"

#include "core1/events.h"

calcap_t g_calcap;

#define START_COUNTS ((int32_t)PHOTON_CAL_SWING_START)
#define NEAR_REST    (START_COUNTS / 2)
// The idle baseline follows thermal drift (minutes), not a key: averaged over
// 1024 sweeps (1.7 s at 600 Hz), a key going down even over seconds stays
// well ahead of it and starts its swing within ~100 ms.
#define REST_SHIFT   10
// Smaller than this and it is not a key press (MIN_EVENT_RANGE, scaled).
#define MIN_SWING    ((int32_t)PHOTON_MIN_EVENT_RANGE << PHOTON_OSR_MODE)
// A swing is over once the key is back within a fifth of its depth: keys
// settle hundreds to thousands of counts away from where they started.
static inline int32_t back_near_rest(uint32_t amp) {
    int32_t near = (int32_t)(amp / 5u);
    return near > NEAR_REST ? near : NEAR_REST;
}

static void set_rest(calcap_key_t *k, uint16_t v) {
    k->rest = v;
    k->rest_fp = (uint32_t)v << REST_SHIFT;
}

static void reset_keys(calcap_t *c) {
    for (int i = 0; i < PHOTON_ACTIVE_SENSORS; i++) {
        calcap_key_t *k = &c->key[i];
        k->state = CALCAP_IDLE;
        k->truncated = 0;
        k->pre_head = 0;
        k->pre_fill = 0;
        k->uncaptured = 0;
        k->unc_amp = 0;
        k->len = 0;
        k->pre_len = 0;
        k->quiet = 0;
        k->rest = 0;
        k->rest_fp = 0;
        k->amp = 0;
        k->swings = 0;
        k->missed = 0;
    }
}

static void start_swing(calcap_key_t *k, uint16_t v, int32_t press, uint32_t now_us) {
    // Pre-roll first, oldest sample first.
    uint32_t n = k->pre_fill;
    for (uint32_t t = 0; t < n; t++) {
        k->samples[t] = k->pre[(k->pre_head + PHOTON_CAL_PRE_SAMPLES - n + t) %
                               PHOTON_CAL_PRE_SAMPLES];
    }
    k->samples[n] = v;
    k->pre_len = (uint16_t)n;
    k->len = (uint16_t)(n + 1);
    // The ring is not fed during the swing: the next pre-roll starts fresh
    // after it, never with samples from before this one.
    k->pre_fill = 0;
    k->pre_head = 0;
    k->truncated = 0;
    k->quiet = 0;
    k->amp = (uint16_t)press;
    k->t_start_us = now_us;
    k->swing_rest = k->rest;
    k->state = CALCAP_ACTIVE;
}

void calcap_sweep(const uint16_t *readings, uint32_t now_us, bool enabled) {
    calcap_t *c = &g_calcap;
    if (enabled != c->enabled) {
        c->enabled = enabled;
        if (enabled) {
            reset_keys(c);
            c->period_us = 0;
            c->last_now_us = now_us;
            __dmb();
            c->session++;
        } else {
            for (int i = 0; i < PHOTON_ACTIVE_SENSORS; i++) {
                if (c->key[i].state == CALCAP_ACTIVE) {
                    c->key[i].state = CALCAP_IDLE;
                }
            }
        }
        return;
    }
    if (!enabled) {
        return;
    }
    uint32_t period = now_us - c->last_now_us;
    c->last_now_us = now_us;
    if (period > 0 && period < 100000u) {
        c->period_us = period;
    }
    uint32_t end_samples = c->period_us
                               ? (PHOTON_CAL_SWING_END_MS * 1000u) / c->period_us
                               : 36u;
    if (end_samples < 4) {
        end_samples = 4;
    }

    for (int i = 0; i < PHOTON_ACTIVE_SENSORS; i++) {
        if ((g_events.disabled_mask >> i) & 1u) {
            continue;
        }
        uint16_t v = readings[i];
        if (v == 0) {
            continue;  // dead read
        }
        calcap_key_t *k = &c->key[i];
        if (k->rest == 0) {
            set_rest(k, v);
        }
        bool inverted = (g_events.polarity_mask >> i) & 1u;
        int32_t press = inverted ? (int32_t)k->rest - v : (int32_t)v - k->rest;

        if (k->state == CALCAP_ACTIVE) {
            if (k->len < PHOTON_CAL_CAP_SAMPLES) {
                k->samples[k->len] = v;
                k->len = (uint16_t)(k->len + 1);
            } else {
                k->truncated = 1;
            }
            if (press > (int32_t)k->amp) {
                k->amp = (uint16_t)(press > 0xFFFF ? 0xFFFF : press);
            }
            if (press < back_near_rest(k->amp)) {
                if (++k->quiet >= end_samples) {
                    k->t_end_us = now_us;
                    k->swings++;
                    set_rest(k, v);  // where the key came back to is its rest now
                    __dmb();
                    k->state = CALCAP_DONE;  // hand over to core 0
                }
            } else {
                k->quiet = 0;
            }
            if (k->state == CALCAP_ACTIVE && (int32_t)k->amp < MIN_SWING &&
                (int32_t)(now_us - k->t_start_us) > 1000000) {
                // A level shift, not a press (a key settling, a neighbour's
                // light): small and not coming back. Drop it, rest is here.
                set_rest(k, v);
                k->state = CALCAP_IDLE;
            }
            continue;
        }

        // IDLE or DONE: watch for the next swing, track the baseline, keep
        // the pre-roll ring full.
        if (press < -START_COUNTS) {
            // Well above the baseline's rest: the key was down when capture
            // started (or the idle level moved). Rest is here.
            set_rest(k, v);
            press = 0;
        }
        if (press > START_COUNTS && k->state == CALCAP_IDLE && !k->uncaptured) {
            start_swing(k, v, press, now_us);
            continue;
        }
        if (press > START_COUNTS && !k->uncaptured) {
            k->uncaptured = 1;  // down while core 0 still has the last swing
            k->unc_amp = 0;
            k->missed++;
        }
        if (k->uncaptured) {
            if (press > (int32_t)k->unc_amp) {
                k->unc_amp = (uint16_t)(press > 0xFFFF ? 0xFFFF : press);
            }
            if (press >= back_near_rest(k->unc_amp)) {
                // Not a pre-roll: the next one starts once this is over.
                k->pre_fill = 0;
                k->pre_head = 0;
                continue;
            }
            k->uncaptured = 0;
            set_rest(k, v);
        }
        if (press < NEAR_REST && press > -START_COUNTS) {
            // Follow slow drift only while the key is at rest (and a key
            // settling back down after a release that re-seated rest high).
            int32_t fp = (int32_t)k->rest_fp;
            fp += (((int32_t)v << REST_SHIFT) - fp) / (1 << REST_SHIFT);
            k->rest_fp = (uint32_t)fp;
            k->rest = (uint16_t)(k->rest_fp >> REST_SHIFT);
        }
        k->pre[k->pre_head] = v;
        k->pre_head = (uint8_t)((k->pre_head + 1) % PHOTON_CAL_PRE_SAMPLES);
        if (k->pre_fill < PHOTON_CAL_PRE_SAMPLES) {
            k->pre_fill++;
        }
    }
}
