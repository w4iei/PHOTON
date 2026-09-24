// Pluck ("knee") detection on one captured key swing. A pure function — no
// hardware, no globals — so the same code runs on a sensor board's core 0
// and in the host unit tests.
//
// As the key goes down the plectrum meets the string and loads it: the key
// slows or creeps while the finger's force builds. When the plectrum lets
// go the load vanishes and the key snaps down. In the position trace that
// is a knee, a sharp rise in slope; the pluck is the last sample before it.
// A fast stroke has no knee (the whole stroke is steep) and reports
// KNEE_NONE: calibration asks for slow presses.
#ifndef PHOTON_KNEE_H
#define PHOTON_KNEE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    KNEE_OK = 0,
    KNEE_SHALLOW,  // too small to be a key press (noise, neighbour crosstalk)
    KNEE_NONE,     // a full press, but no knee inside the window
} knee_reason_t;

// The tunable part of the shape (console 'cal rules', saved per board).
typedef struct {
    uint8_t lo_pct;     // knee searched within lo..hi % of the swing's range
    uint8_t hi_pct;
    uint8_t ratio_x10;  // the snap is this many times steeper than the approach
    uint8_t jump_pct;   // and carries this % of the range within 15 ms
} knee_rules_t;

typedef struct {
    uint8_t reason;   // knee_reason_t
    uint8_t pct;      // knee position, % of the swing's own rest..top range
    uint16_t idx;     // sample index of the knee
    uint16_t value;   // raw reading at the knee
    uint16_t rest;    // rest level used (given, or the older half of the pre-roll)
    uint16_t top;     // swing's deepest level (max of a 3-sample mean), raw
} knee_result_t;

// s: raw readings, n samples period_us apart, the first `pre` taken before
// the swing started. rest: the key's idle reading if known (the capture's
// baseline), 0 = estimate it from the pre-roll. inverted: pressed = lower
// reading. A swing whose range is below min_range counts as KNEE_SHALLOW.
knee_result_t knee_find(const uint16_t *s, uint32_t n, uint32_t pre, uint16_t rest,
                        uint32_t period_us, bool inverted, const knee_rules_t *rules,
                        uint32_t min_range);

#endif
