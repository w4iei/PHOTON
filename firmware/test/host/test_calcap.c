// Calibration capture (core 1) tests: a swing is recorded with its pre-roll
// and handed to core 0, keys are independent, a swing that starts while
// its key is still with core 0 is counted as missed, and switching capture
// on/off starts a session / abandons what is in progress.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core1/calcap.h"
#include "core1/events.h"

photon_event_ring_t g_event_ring;
photon_snapshot_t g_snapshot;
photon_cmd_mailbox_t g_cmd_mailbox;
photon_trace_ring_t g_trace_ring;

static int checks = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

#define PERIOD 1667u
#define REST   2000

static uint16_t readings[PHOTON_MAX_SENSORS];
static uint32_t now;

static void setup(uint32_t enabled_mask) {
    memset(&g_calcap, 0, sizeof g_calcap);
    events_init(~enabled_mask);
    for (int i = 0; i < PHOTON_MAX_SENSORS; i++) {
        readings[i] = REST;
    }
    now = 0;
    calcap_sweep(readings, now, true);  // capture on: new session
}

static void sweep(void) {
    now += PERIOD;
    calcap_sweep(readings, now, true);
}

static void hold(int sweeps) {
    for (int s = 0; s < sweeps; s++) {
        sweep();
    }
}

// Key k from rest down to `to` over `sweeps` sweeps.
static void ramp(int k, int to, int sweeps) {
    int from = readings[k];
    for (int s = 1; s <= sweeps; s++) {
        readings[k] = (uint16_t)(from + (to - from) * s / sweeps);
        sweep();
    }
}

static void test_one_swing(void) {
    setup(1u << 0);
    CHECK(g_calcap.session == 1);
    hold(50);                       // pre-roll fills (32 kept)
    ramp(0, 8000, 30);
    hold(40);
    CHECK(g_calcap.key[0].state == CALCAP_ACTIVE);
    ramp(0, REST, 20);
    hold(40);                       // 60 ms back near rest ends it
    const calcap_key_t *k = &g_calcap.key[0];
    CHECK(k->state == CALCAP_DONE);
    CHECK(k->swings == 1);
    CHECK(k->pre_len == PHOTON_CAL_PRE_SAMPLES);
    // Pre-roll: the samples before the swing crossed the start threshold,
    // which may already include the first small step of the press.
    CHECK(k->samples[0] == REST);
    for (int i = 0; i < PHOTON_CAL_PRE_SAMPLES; i++) {
        CHECK(k->samples[i] <= REST + PHOTON_CAL_SWING_START);
    }
    CHECK(k->samples[k->pre_len] > REST + PHOTON_CAL_SWING_START);  // first swing sample
    CHECK(k->amp == 6000);
    CHECK(k->len > k->pre_len + 30 + 40 + 20 && k->len < k->pre_len + 30 + 40 + 20 + 40);
    CHECK(!k->truncated);
    CHECK((int32_t)(k->t_end_us - k->t_start_us) > 0);
    CHECK(g_calcap.period_us == PERIOD);
}

static void test_keys_independent_and_disabled_ignored(void) {
    setup((1u << 0) | (1u << 2));   // key 1 disabled
    hold(40);
    readings[0] = 8000;
    readings[1] = 8000;
    readings[2] = 8000;
    hold(20);
    CHECK(g_calcap.key[0].state == CALCAP_ACTIVE);
    CHECK(g_calcap.key[1].state == CALCAP_IDLE);  // disabled: never captured
    CHECK(g_calcap.key[2].state == CALCAP_ACTIVE);
    readings[0] = REST;
    hold(40);
    CHECK(g_calcap.key[0].state == CALCAP_DONE);
    CHECK(g_calcap.key[2].state == CALCAP_ACTIVE);
}

static void test_swing_while_done_is_missed(void) {
    setup(1u << 0);
    hold(40);
    ramp(0, 8000, 10);
    ramp(0, REST, 10);
    hold(40);
    CHECK(g_calcap.key[0].state == CALCAP_DONE);
    ramp(0, 8000, 10);              // core 0 has not taken the first one yet
    CHECK(g_calcap.key[0].missed == 1);
    CHECK(g_calcap.key[0].swings == 1);
    ramp(0, REST, 10);
    hold(10);
    g_calcap.key[0].state = CALCAP_IDLE;  // core 0 hands the buffer back
    hold(5);
    ramp(0, 8000, 10);
    CHECK(g_calcap.key[0].state == CALCAP_ACTIVE);
    // The pre-roll holds only samples from after the missed swing was back
    // near rest (within a fifth of its depth), never its deep part.
    CHECK(g_calcap.key[0].pre_len >= 16);
    for (int i = 0; i < g_calcap.key[0].pre_len; i++) {
        CHECK(g_calcap.key[0].samples[i] <= REST + 6000 / 5);
    }
}

static void test_truncated(void) {
    setup(1u << 0);
    hold(40);
    readings[0] = 8000;
    hold(PHOTON_CAL_CAP_SAMPLES + 100);
    CHECK(g_calcap.key[0].len == PHOTON_CAL_CAP_SAMPLES);
    CHECK(g_calcap.key[0].truncated);
    readings[0] = REST;
    hold(40);
    CHECK(g_calcap.key[0].state == CALCAP_DONE);
}

static void test_session_toggle(void) {
    setup(1u << 0);
    hold(40);
    readings[0] = 8000;
    hold(5);
    CHECK(g_calcap.key[0].state == CALCAP_ACTIVE);
    now += PERIOD;
    calcap_sweep(readings, now, false);          // calibration frozen
    CHECK(g_calcap.key[0].state == CALCAP_IDLE); // in-progress swing abandoned
    uint32_t len = g_calcap.key[0].len;
    now += PERIOD;
    calcap_sweep(readings, now, false);
    CHECK(g_calcap.key[0].len == len);           // nothing captured while off
    now += PERIOD;
    calcap_sweep(readings, now, true);           // new calibration
    CHECK(g_calcap.session == 2);
    CHECK(g_calcap.key[0].swings == 0 && g_calcap.key[0].len == 0);
}

static void test_dead_reads_skipped(void) {
    setup(1u << 0);
    hold(40);
    readings[0] = 0;                // dead read: not a swing, not a sample
    hold(3);
    CHECK(g_calcap.key[0].state == CALCAP_IDLE);
    readings[0] = REST;
    hold(3);
    CHECK(g_calcap.key[0].rest == REST);
}

static void test_key_down_at_start(void) {
    setup(1u << 0);
    readings[0] = 8000;             // held down as calibration starts
    hold(20);
    CHECK(g_calcap.key[0].state == CALCAP_IDLE);
    readings[0] = REST;             // let up: that is rest
    hold(40);
    CHECK(g_calcap.key[0].rest == REST);
    ramp(0, 8000, 10);
    CHECK(g_calcap.key[0].state == CALCAP_ACTIVE);  // the next press is captured
}

static void test_settles_higher(void) {
    // A 35000-count press that comes back 3000 counts above where it started
    // (felt, a slow return; seen on the upper manual): the swing still ends,
    // and that level is rest.
    setup(1u << 0);
    hold(40);
    ramp(0, REST + 35000, 20);
    ramp(0, REST + 3000, 20);
    hold(40);
    CHECK(g_calcap.key[0].state == CALCAP_DONE);
    CHECK(g_calcap.key[0].rest == REST + 3000);
    g_calcap.key[0].state = CALCAP_IDLE;
    hold(40);
    CHECK(g_calcap.key[0].state == CALCAP_IDLE);   // resting there is not a swing
    ramp(0, REST + 35000, 20);
    CHECK(g_calcap.key[0].state == CALCAP_ACTIVE);  // the next press is
    CHECK(g_calcap.key[0].swing_rest == REST + 3000);
    // Settling back down to the old level is followed, not taken for a swing.
    ramp(0, REST + 3000, 20);
    hold(40);
    g_calcap.key[0].state = CALCAP_IDLE;
    readings[0] = REST + 2800;
    hold(3000);
    CHECK(g_calcap.key[0].rest < REST + 2850 && g_calcap.key[0].state == CALCAP_IDLE);
}

static void test_level_shift_dropped(void) {
    // A 500-count step that stays (a key settling, a neighbour's light):
    // starts a swing, is too small to be a press, dropped after a second.
    setup(1u << 0);
    hold(40);
    readings[0] = REST + 500;
    hold(10);
    CHECK(g_calcap.key[0].state == CALCAP_ACTIVE);
    hold(650);
    CHECK(g_calcap.key[0].state == CALCAP_IDLE);
    CHECK(g_calcap.key[0].rest == REST + 500);
    CHECK(g_calcap.key[0].swings == 0);
    ramp(0, 8000, 20);
    CHECK(g_calcap.key[0].state == CALCAP_ACTIVE);  // a real press after it
}

int main(void) {
    test_one_swing();
    test_keys_independent_and_disabled_ignored();
    test_swing_while_done_is_missed();
    test_truncated();
    test_session_toggle();
    test_dead_reads_skipped();
    test_key_down_at_start();
    test_settles_higher();
    test_level_shift_dropped();
    printf("test_calcap OK (%d checks)\n", checks);
    return 0;
}
