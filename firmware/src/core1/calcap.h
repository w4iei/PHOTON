// Calibration swing capture (core 1). While calibration learning is on,
// every key's swing — rest, down, back to rest — is recorded into that key's
// own buffer, starting with a pre-roll from before it began, so any number
// of keys can be captured at once. Core 0 (cal/cal_session.c) analyses each
// finished swing and hands the buffer back.
//
// Ownership per key: IDLE and ACTIVE belong to core 1; DONE belongs to
// core 0 until it sets the key back to IDLE. A swing that starts while its
// key is still DONE is not captured (counted in `missed`).
#ifndef PHOTON_CALCAP_H
#define PHOTON_CALCAP_H

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"

typedef enum {
    CALCAP_IDLE = 0,  // watching for a swing to start
    CALCAP_ACTIVE,    // swing in progress, core 1 appending samples
    CALCAP_DONE,      // swing finished, waiting for core 0
} calcap_state_t;

typedef struct {
    volatile uint8_t state;       // calcap_state_t
    volatile uint8_t truncated;   // swing outlasted the buffer (tail missing)
    uint8_t pre_head;             // pre-roll ring write index
    uint8_t pre_fill;
    uint8_t uncaptured;           // in a swing that started while DONE
    uint16_t unc_amp;             // ... and its depth so far
    volatile uint16_t len;        // samples in samples[]
    volatile uint16_t pre_len;    // of which pre-roll
    uint16_t quiet;               // consecutive samples back near rest
    uint16_t rest;                // idle baseline (0 = not seeded yet)
    volatile uint16_t swing_rest; // the baseline when this swing started
    uint32_t rest_fp;             // ... in 22.10 fixed point, for its slow average
    volatile uint16_t amp;        // deepest press this swing, counts above rest
    volatile uint32_t t_start_us;
    volatile uint32_t t_end_us;
    volatile uint32_t swings;     // finished swings
    volatile uint32_t missed;     // swings not captured
    uint16_t pre[PHOTON_CAL_PRE_SAMPLES];
    uint16_t samples[PHOTON_CAL_CAP_SAMPLES];
} calcap_key_t;

typedef struct {
    calcap_key_t key[PHOTON_ACTIVE_SENSORS];
    volatile uint32_t period_us;  // measured sweep period while capturing
    volatile uint32_t session;    // bumped each time capture switches on
    uint32_t last_now_us;
    bool enabled;
} calcap_t;

extern calcap_t g_calcap;

// Core 1, once per sweep after events_process(). `enabled` follows
// calibration learning; switching it on starts a new session (all keys
// reset), switching it off abandons swings still in progress.
void calcap_sweep(const uint16_t *readings, uint32_t now_us, bool enabled);

#endif
