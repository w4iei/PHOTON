// Knee detector tests on synthetic swings at the 600 Hz production rate:
// a slow press finds its pluck, and the look-alikes (fast stroke, finger
// speeding up, action disengaged, knee outside the window, a shallow
// crosstalk bump) do not. Shapes follow the January 2026 sensor traces.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cal/knee.h"

static int checks = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

#define PERIOD_US 1667u
#define PRE       32u
#define REST      2000
#define RANGE     6000
#define MIN_RANGE 1360u

typedef struct {
    float ms;   // duration of a linear move ...
    float pct;  // ... to this depth, % of RANGE
} seg_t;

static uint16_t buf[4096];
static uint32_t lcg;

static int noise(int amp) {
    if (amp == 0) {
        return 0;
    }
    lcg = lcg * 1103515245u + 12345u;
    return (int)((lcg >> 16) % (uint32_t)(2 * amp + 1)) - amp;
}

static uint16_t level(float pct, bool inverted, int noise_amp) {
    int depth = (int)(pct / 100.0f * RANGE + 0.5f) + noise(noise_amp);
    return (uint16_t)(inverted ? 60000 - depth : REST + depth);
}

// Pre-roll at rest, then the segments. Returns the sample count; *ends
// receives the index of the last sample of each segment.
static uint32_t build(const seg_t *segs, int nseg, bool inverted, int noise_amp,
                      uint32_t *ends) {
    uint32_t n = 0;
    lcg = 12345u;
    for (uint32_t i = 0; i < PRE; i++) {
        buf[n++] = level(0.0f, inverted, noise_amp);
    }
    float at = 0.0f;
    for (int s = 0; s < nseg; s++) {
        uint32_t steps = (uint32_t)(segs[s].ms * 1000.0f / PERIOD_US + 0.5f);
        if (steps == 0) {
            steps = 1;
        }
        for (uint32_t k = 1; k <= steps; k++) {
            buf[n++] = level(at + (segs[s].pct - at) * (float)k / (float)steps, inverted,
                             noise_amp);
        }
        at = segs[s].pct;
        if (ends) {
            ends[s] = n - 1;
        }
    }
    return n;
}

// The compiled defaults: window 60 +/- 20%, snap 2.5x the approach and 8%.
static const knee_rules_t rules = { 40, 80, 25, 8 };

static knee_result_t find(uint32_t n, bool inverted) {
    return knee_find(buf, n, PRE, 0, PERIOD_US, inverted, &rules, MIN_RANGE);
}

// Slow press: approach, the plectrum loads the string (creep 55 -> 63%),
// pluck, the key snaps to the bottom, held, released.
static const seg_t slow_press[] = {
    { 300, 55 }, { 300, 63 }, { 8, 95 }, { 400, 97 }, { 150, 0 }, { 100, 0 },
};

static void test_slow_press(void) {
    uint32_t ends[6];
    uint32_t n = build(slow_press, 6, false, 0, ends);
    knee_result_t r = find(n, false);
    CHECK(r.reason == KNEE_OK);
    CHECK(r.idx == ends[1]);  // the last sample before the snap
    CHECK(r.pct >= 63 && r.pct <= 66);  // 63% of RANGE = 65% of the swing's 97% depth
    CHECK(r.value == buf[ends[1]]);
    CHECK(r.rest == REST);
}

static void test_long_creep(void) {
    // A very slow press: 1.2 s approach, the string loading for a full
    // second, then the snap.
    static const seg_t s[] = { { 1200, 55 }, { 1000, 63 }, { 8, 97 }, { 300, 97 }, { 150, 0 } };
    uint32_t ends[5];
    uint32_t n = build(s, 5, false, 60, ends);
    knee_result_t r = find(n, false);
    CHECK(r.reason == KNEE_OK);
    // On a creep this flat the noise decides the exact sample (within 10 ms);
    // the level there, which sets the threshold, is the same.
    CHECK(r.idx + 6 >= ends[1] && r.idx <= ends[1] + 1);
    CHECK(r.pct >= 63 && r.pct <= 66);
}

static void test_slow_press_noisy(void) {
    // ~1% of range noise, like the production floor (62 counts on 6,900).
    uint32_t ends[6];
    uint32_t n = build(slow_press, 6, false, 60, ends);
    knee_result_t r = find(n, false);
    CHECK(r.reason == KNEE_OK);
    CHECK(r.idx + 2 >= ends[1] && r.idx <= ends[1] + 1);
}

static void test_given_rest(void) {
    // A pre-roll that already starts partway down (a very slow approach):
    // with the true rest given, the knee still reads 65% of the swing.
    uint32_t ends[6];
    uint32_t n = build(slow_press, 6, false, 0, ends);
    for (uint32_t i = 0; i < PRE; i++) {
        buf[i] = (uint16_t)(REST + 150 + 5 * i);
    }
    knee_result_t r = knee_find(buf, n, PRE, REST, PERIOD_US, false, &rules, MIN_RANGE);
    CHECK(r.reason == KNEE_OK && r.rest == REST);
    CHECK(r.pct >= 64 && r.pct <= 66);
}

static void test_inverted(void) {
    uint32_t ends[6];
    uint32_t n = build(slow_press, 6, true, 0, ends);
    knee_result_t r = find(n, true);
    CHECK(r.reason == KNEE_OK);
    CHECK(r.idx == ends[1]);
    CHECK(r.rest == 60000 && r.top < r.rest);
}

static void test_fast_stroke(void) {
    // The whole stroke is steep: nothing to find, the key is replayed slower.
    static const seg_t s[] = { { 30, 100 }, { 300, 100 }, { 60, 0 }, { 100, 0 } };
    uint32_t n = build(s, 4, false, 0, NULL);
    CHECK(find(n, false).reason == KNEE_NONE);
}

static void test_disengaged(void) {
    // Action disengaged: a slow, even swing with no string to pluck.
    static const seg_t s[] = { { 500, 100 }, { 300, 100 }, { 200, 0 }, { 100, 0 } };
    uint32_t n = build(s, 4, false, 0, NULL);
    CHECK(find(n, false).reason == KNEE_NONE);
}

static void test_finger_speeding_up(void) {
    // A finger speeding up smoothly (1.5x every 6 ms, 0.2 -> 2.3% of travel
    // per ms) through the window; a pluck changes speed within a sample or
    // two. Against the 20 ms approach alone this ramp would pass for a knee.
    static const seg_t s[] = {
        { 300, 45 },  { 6, 46.2f }, { 6, 48.0f }, { 6, 50.7f }, { 6, 54.75f },
        { 6, 60.75f }, { 6, 69.75f }, { 6, 83.25f }, { 10, 100 }, { 300, 100 }, { 100, 0 },
    };
    uint32_t n = build(s, 11, false, 0, NULL);
    CHECK(find(n, false).reason == KNEE_NONE);
}

static void test_soft_snap(void) {
    // The upper manual's D2 (2026-09-24): after the creep the snap builds up
    // over ~13 ms instead of in one sample. The knee is where it starts, 51%.
    static const seg_t s[] = {
        { 300, 45 }, { 200, 51 },
        { 1.67f, 52 }, { 1.67f, 53 }, { 1.67f, 54 }, { 1.67f, 56 }, { 1.67f, 58 },
        { 1.67f, 61 }, { 1.67f, 65 }, { 1.67f, 71 }, { 1.67f, 76 }, { 1.67f, 82 },
        { 1.67f, 88 }, { 1.67f, 93 }, { 1.67f, 98 }, { 1.67f, 100 }, { 300, 100 }, { 100, 0 },
    };
    uint32_t ends[18];
    uint32_t n = build(s, 18, false, 0, ends);
    knee_result_t r = find(n, false);
    CHECK(r.reason == KNEE_OK);
    CHECK(r.idx + 1 >= ends[1] && r.idx <= ends[1] + 3);
    CHECK(r.pct >= 51 && r.pct <= 54);
}

static void test_slow_speedup_after_creep(void) {
    // After the creep the key picks up speed over ~45 ms (1.3x every 5 ms):
    // a finger, not a snap.
    static const seg_t s[] = {
        { 300, 55 }, { 5, 56.2f }, { 5, 57.7f }, { 5, 59.6f }, { 5, 62.1f }, { 5, 65.4f },
        { 5, 69.7f }, { 5, 75.2f }, { 5, 82.4f }, { 5, 91.9f }, { 5, 100 }, { 300, 100 }, { 100, 0 },
    };
    uint32_t n = build(s, 13, false, 0, NULL);
    CHECK(find(n, false).reason == KNEE_NONE);
}

static void test_small_step_is_not_the_pluck(void) {
    // A quick 6% step (the plectrum slipping, a damper touching) is steep
    // and stays, but carries too little: the pluck is the snap that follows.
    static const seg_t s[] = {
        { 300, 55 }, { 3.3f, 61 }, { 300, 63 }, { 6, 95 }, { 300, 97 }, { 100, 0 },
    };
    uint32_t ends[6];
    uint32_t n = build(s, 6, false, 0, ends);
    knee_result_t r = find(n, false);
    CHECK(r.reason == KNEE_OK);
    CHECK(r.idx == ends[2]);
}

static void test_glitch_is_not_the_pluck(void) {
    // One sample reading 20% high during the creep (a bus glitch): steep and
    // large, but the key is not down, so the next sample is back.
    static const seg_t s[] = {
        { 300, 55 }, { 150, 58 }, { 150, 63 }, { 8, 95 }, { 300, 97 }, { 100, 0 },
    };
    uint32_t ends[6];
    uint32_t n = build(s, 6, false, 0, ends);
    buf[ends[1] + 1] = (uint16_t)(buf[ends[1] + 1] + RANGE / 5);
    knee_result_t r = find(n, false);
    CHECK(r.reason == KNEE_OK);
    CHECK(r.idx == ends[2]);
}

static void test_knee_below_window(void) {
    // Plucks at 35%, below the window: not reported at the window's edge as
    // the snap passes through it. With a wider window it is found where it is.
    static const seg_t s[] = { { 300, 30 }, { 300, 35 }, { 8, 97 }, { 300, 97 }, { 150, 0 } };
    uint32_t ends[5];
    uint32_t n = build(s, 5, false, 0, ends);
    CHECK(find(n, false).reason == KNEE_NONE);
    knee_rules_t wide = { 30, 90, 25, 8 };
    knee_result_t r = knee_find(buf, n, PRE, 0, PERIOD_US, false, &wide, MIN_RANGE);
    CHECK(r.reason == KNEE_OK && r.idx == ends[1]);
}

static void test_fast_stroke_with_hitch(void) {
    // A fast stroke that hesitates for a few ms where the plectrum meets the
    // string. Possibly a pluck, but with no slow loading phase before it the
    // shape is not trusted: calibration asks for that key again, slower.
    static const seg_t s[] = {
        { 20, 50 }, { 5, 51 }, { 12, 97 }, { 300, 97 }, { 100, 0 },
    };
    uint32_t n = build(s, 5, false, 0, NULL);
    CHECK(find(n, false).reason == KNEE_NONE);
}

static void test_knee_above_window(void) {
    static const seg_t s[] = { { 400, 78 }, { 200, 80 }, { 8, 98 }, { 300, 98 }, { 100, 0 } };
    uint32_t n = build(s, 5, false, 0, NULL);
    CHECK(find(n, false).reason == KNEE_NONE);
}

static void test_first_knee_wins(void) {
    // Two knees in the window: the first is the pluck that sounds first.
    static const seg_t s[] = {
        { 300, 50 }, { 200, 56 }, { 6, 68 }, { 200, 70 }, { 6, 97 }, { 300, 97 },
    };
    uint32_t ends[6];
    uint32_t n = build(s, 6, false, 0, ends);
    knee_result_t r = find(n, false);
    CHECK(r.reason == KNEE_OK);
    CHECK(r.idx == ends[1]);
}

static void test_shallow(void) {
    // A neighbour's cross-illumination: a bump well under a key's travel.
    static const seg_t s[] = { { 100, 15 }, { 100, 15 }, { 100, 0 } };
    uint32_t n = build(s, 3, false, 0, NULL);
    CHECK(find(n, false).reason == KNEE_SHALLOW);
}

static void test_degenerate_inputs(void) {
    CHECK(knee_find(NULL, 100, 10, 0, PERIOD_US, false, &rules, MIN_RANGE).reason ==
          KNEE_SHALLOW);
    CHECK(knee_find(buf, 3, 1, 0, PERIOD_US, false, &rules, MIN_RANGE).reason ==
          KNEE_SHALLOW);
    CHECK(knee_find(buf, 100, 10, 0, 0, false, &rules, MIN_RANGE).reason == KNEE_SHALLOW);
    CHECK(knee_find(buf, 100, 10, 0, PERIOD_US, false, NULL, MIN_RANGE).reason ==
          KNEE_SHALLOW);
}

int main(void) {
    test_slow_press();
    test_slow_press_noisy();
    test_long_creep();
    test_inverted();
    test_given_rest();
    test_fast_stroke();
    test_disengaged();
    test_finger_speeding_up();
    test_soft_snap();
    test_slow_speedup_after_creep();
    test_small_step_is_not_the_pluck();
    test_glitch_is_not_the_pluck();
    test_knee_below_window();
    test_fast_stroke_with_hitch();
    test_knee_above_window();
    test_first_knee_wins();
    test_shallow();
    test_degenerate_inputs();
    printf("test_knee OK (%d checks)\n", checks);
    return 0;
}
