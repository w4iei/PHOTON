// Event-engine tests: seeded-calibration strike/release with exact dt
// assertions, hysteresis disarm, range gate, and the disabled-sensor mask.
// (The legacy activation flag and boot auto-disable heuristic are
// intentionally retired — see docs/architecture/01 §7.)
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core1/events.h"

// events.c pushes into the event ring; rings.c is not compiled on the host,
// so provide the globals here.
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

static uint16_t readings[PHOTON_MAX_SENSORS];

static void feed(uint16_t sensor0_value, uint32_t t_us) {
    readings[0] = sensor0_value;
    events_process(readings, t_us);
}

static uint32_t ring_count(void) {
    return g_event_ring.tail - g_event_ring.head;
}

static photon_event_t pop(void) {
    photon_event_t ev = g_event_ring.slots[g_event_ring.head % PHOTON_EVENT_RING_SLOTS];
    g_event_ring.head++;
    return ev;
}

// Seeded cal min=1000 max=3000 (range 2000 >= 1360 gate). Thresholds:
// vel_start 30% -> press 600 (value 1600), strike 60% -> 1200 (value 2200),
// release-arm 80% -> 1600 (value 2600), release 40% -> 800 (value 1800).
static void setup(void) {
    memset(&g_event_ring, 0, sizeof g_event_ring);
    memset(readings, 0, sizeof readings);
    events_init(0xFFFFFFFEu);  // only sensor 0 enabled
    events_seed_cal(0, 1000, 3000);
}

static void test_strike_release_dt(void) {
    setup();
    feed(1000, 0);          // idle
    feed(1500, 1000);       // press 500 < 600: still idle
    CHECK(ring_count() == 0);
    feed(1700, 2000);       // press 700: arms, t=2000
    CHECK(ring_count() == 0);
    feed(2000, 2500);       // press 1000: still below strike 1200
    feed(2300, 3000);       // press 1300 >= 1200: ON, dt = 3000-2000
    CHECK(ring_count() == 1);
    photon_event_t ev = pop();
    CHECK(ev.local_idx == 0 && ev.state == 1 && ev.dt_us == 1000 && ev.seq == 0);

    feed(2900, 4000);       // fully pressed (press 1900 > rel_vel 1600): no arm
    feed(2500, 5000);       // press 1500 <= 1600: release arms, t=5000
    CHECK(ring_count() == 0);
    feed(1700, 6500);       // press 700 <= 800: OFF, dt = 6500-5000
    CHECK(ring_count() == 1);
    ev = pop();
    CHECK(ev.state == 0 && ev.dt_us == 1500 && ev.seq == 1);
}

static void test_strike_disarm_hysteresis(void) {
    setup();
    feed(1000, 0);
    feed(1700, 1000);       // arm
    feed(1200, 2000);       // press 200 < 600: disarm (key bounced back)
    feed(2300, 3000);       // jumps straight past strike: arm+fire same sweep
    CHECK(ring_count() == 1);
    photon_event_t ev = pop();
    CHECK(ev.state == 1 && ev.dt_us == 0);  // both crossings in one sweep
}

static void test_range_gate(void) {
    setup();
    events_reset_cal();          // no calibration: learn from signal
    g_events.learning = true;    // calibration mode
    // Wiggle inside a small range (< 1360 scaled gate): must never fire.
    for (int i = 0; i < 100; i++) {
        feed((uint16_t)(1000 + (i % 2) * 500), (uint32_t)i * 1000);
    }
    CHECK(ring_count() == 0);
}

static void test_frozen_empty_cal_inert(void) {
    setup();
    events_reset_cal();          // min=0xFFFF > max=0
    g_events.learning = false;   // frozen with no calibration
    // The unsigned min/max underflow must not fabricate a range: no events,
    // and no learning happens while frozen.
    for (int i = 0; i < 50; i++) {
        feed((uint16_t)(1000 + (i % 2) * 3000), (uint32_t)i * 1000);
    }
    CHECK(ring_count() == 0);
    CHECK(g_events.min[0] == 0xFFFF && g_events.max[0] == 0);
}

static void test_zero_reading_ignored(void) {
    setup();
    feed(1000, 0);
    feed(0, 1000);          // dead read: must not disturb calibration
    CHECK(g_events.min[0] == 1000);
    feed(2300, 2000);
    feed(2300, 3000);
    CHECK(ring_count() == 1);  // still fires normally afterwards
}

static void test_disabled_mask(void) {
    memset(&g_event_ring, 0, sizeof g_event_ring);
    memset(readings, 0, sizeof readings);
    events_init(1u << 0);  // sensor 0 disabled
    events_seed_cal(0, 1000, 3000);
    feed(1000, 0);
    feed(2300, 1000);
    feed(2300, 2000);
    CHECK(ring_count() == 0);          // disabled sensor never fires
    CHECK(g_events.value[0] == 0);     // and reports 0
}

int main(void) {
    test_strike_release_dt();
    test_strike_disarm_hysteresis();
    test_range_gate();
    test_frozen_empty_cal_inert();
    test_zero_reading_ignored();
    test_disabled_mask();
    printf("test_events OK (%d checks)\n", checks);
    return 0;
}
