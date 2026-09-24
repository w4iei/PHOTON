// Knee calibration session tests (core 0), fed through the real core-1
// capture: a slow press goes green, a fast one asks to be redone slower,
// neighbours pressed together are both redone, a neighbour's crosstalk is
// ignored, the save turns knees into per-key strike thresholds (and does
// nothing on a board that was not calibrating), and the wire payloads carry
// the before/after values and the kept swings.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cal/cal_session.h"
#include "config/config_store.h"
#include "core1/calcap.h"
#include "core1/events.h"

photon_event_ring_t g_event_ring;
photon_snapshot_t g_snapshot;
photon_cmd_mailbox_t g_cmd_mailbox;
photon_trace_ring_t g_trace_ring;
photon_config_t g_config;
uint8_t g_strike_stage[PHOTON_MAX_SENSORS];

void snapshot_read(photon_snapshot_t *out) { *out = g_snapshot; }

static photon_cmd_t last_cmd;
static int cmds_pushed;
bool cmd_mailbox_push(const photon_cmd_t *cmd) {
    last_cmd = *cmd;
    cmds_pushed++;
    return true;
}

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
#define RANGE  6000

static uint16_t readings[PHOTON_MAX_SENSORS];
static uint32_t now;

static void sweep(void) {
    now += PERIOD;
    calcap_sweep(readings, now, g_events.learning);
    cal_session_task();
}

static void hold(int sweeps) {
    for (int s = 0; s < sweeps; s++) {
        sweep();
    }
}

typedef struct {
    float ms;
    float pct;
} seg_t;

// Move every key in `keys` through the segments together (depth % of RANGE).
static void play(uint32_t keys, const seg_t *segs, int nseg) {
    float at = 0.0f;
    for (int s = 0; s < nseg; s++) {
        int steps = (int)(segs[s].ms * 1000.0f / PERIOD + 0.5f);
        if (steps < 1) {
            steps = 1;
        }
        for (int k = 1; k <= steps; k++) {
            float pct = at + (segs[s].pct - at) * (float)k / (float)steps;
            for (int i = 0; i < PHOTON_ACTIVE_SENSORS; i++) {
                if ((keys >> i) & 1u) {
                    readings[i] = (uint16_t)(REST + (int)(pct / 100.0f * RANGE + 0.5f));
                }
            }
            sweep();
        }
        at = segs[s].pct;
    }
    hold(60);  // back at rest long enough to end the swing
}

// Slow press, pluck at 63% of RANGE; the key bottoms at 97%.
static const seg_t slow[] = { { 300, 55 }, { 300, 63 }, { 8, 97 }, { 300, 97 }, { 150, 0 } };
static const seg_t fast[] = { { 30, 97 }, { 200, 97 }, { 60, 0 } };
static const seg_t bump[] = { { 100, 25 }, { 100, 25 }, { 100, 0 } };  // crosstalk-sized

static void setup(void) {
    // Fresh capture state, but the session counter only ever goes up (on the
    // board it is never reset), so each test starts a new calibration.
    uint32_t session = g_calcap.session;
    memset(&g_calcap, 0, sizeof g_calcap);
    g_calcap.session = session;
    memset(&g_config, 0, sizeof g_config);
    memset(&g_snapshot, 0, sizeof g_snapshot);
    for (int i = 0; i < PHOTON_MAX_SENSORS; i++) {
        g_config.cal_min[i] = 1900;     // the calibration in force before
        g_config.cal_max[i] = 7900;
        g_snapshot.min[i] = REST;       // what learning finds this time
        g_snapshot.max[i] = REST + RANGE;
        readings[i] = REST;
    }
    g_config.strike_pct[3] = 70;        // one key already had its own threshold
    events_init(~0x3Fu);                // keys 0-5 populated; key 5 disabled below
    g_events.disabled_mask |= 1u << 5;
    g_events.learning = true;           // 'r' / 'cal reset'
    now = 0;
    cmds_pushed = 0;
    hold(40);                           // capture on, pre-rolls fill
}

static void test_session(void) {
    setup();
    CHECK(cal_session_active());
    CHECK(cal_session_have());
    CHECK(cal_session_status(0) == CAL_KEY_UNSEEN);
    CHECK(cal_session_status(5) == CAL_KEY_DISABLED);

    play(1u << 0, slow, 5);
    CHECK(cal_session_status(0) == CAL_KEY_OK);
    CHECK(g_calcap.key[0].state == CALCAP_IDLE);  // buffer handed back
    const cal_key_t *k0 = cal_session_key(0);
    CHECK(k0->n_knees == 1);
    CHECK(k0->kept_flags == (CAL_SWING_VALID | CAL_SWING_KNEE));
    // Knee at 63% of the 2000..8000 calibrated range, +3% margin.
    CHECK(cal_session_strike(0, REST, REST + RANGE) == 66);

    play(1u << 2, fast, 3);
    CHECK(cal_session_status(2) == CAL_KEY_NO_KNEE);
    CHECK(cal_session_key(2)->kept_flags == CAL_SWING_VALID);  // kept, no knee
    CHECK(cal_session_strike(2, REST, REST + RANGE) == 0);

    // Neighbours together: both redone, neither knee counted.
    play((1u << 3) | (1u << 4), slow, 5);
    CHECK(cal_session_status(3) == CAL_KEY_ADJACENT);
    CHECK(cal_session_status(4) == CAL_KEY_ADJACENT);
    CHECK(cal_session_key(3)->n_knees == 0);
    CHECK(cal_session_key(3)->kept_flags & CAL_SWING_ADJACENT);
    // Then key 3 alone.
    play(1u << 3, slow, 5);
    CHECK(cal_session_status(3) == CAL_KEY_OK);
    CHECK(cal_session_status(4) == CAL_KEY_ADJACENT);

    // Two keys apart may go down together.
    play((1u << 0) | (1u << 2), slow, 5);
    CHECK(cal_session_status(0) == CAL_KEY_OK);
    CHECK(cal_session_key(0)->n_knees == 2);
    CHECK(cal_session_status(2) == CAL_KEY_OK);

    // A green key stays green through a bad press.
    play(1u << 0, fast, 3);
    CHECK(cal_session_status(0) == CAL_KEY_OK);
    CHECK(cal_session_key(0)->n_knees == 2);

    // Crosstalk-sized bump on a key never pressed: ignored.
    play(1u << 1, bump, 3);
    CHECK(cal_session_status(1) == CAL_KEY_UNSEEN);
    CHECK(g_calcap.key[1].state == CALCAP_IDLE);

    // Live view payload.
    uint8_t buf[PHOTON_FRAME_MAX_PAYLOAD];
    uint8_t len = cal_session_build_status(buf);
    CHECK(len == 2 + 2 * PHOTON_ACTIVE_SENSORS);
    CHECK(buf[0] == 1 && buf[1] == PHOTON_ACTIVE_SENSORS);
    CHECK(buf[2 + 0] == CAL_KEY_OK && buf[2 + 1] == CAL_KEY_UNSEEN &&
          buf[2 + 4] == CAL_KEY_ADJACENT && buf[2 + 5] == CAL_KEY_DISABLED);
    CHECK(buf[2 + PHOTON_ACTIVE_SENSORS + 0] == 66);  // what a save would store
    CHECK(buf[2 + PHOTON_ACTIVE_SENSORS + 4] == 0);

    // Before / after payload.
    len = cal_session_build_info(0, CAL_INFO_PER_FRAME, buf);
    CHECK(len == 3 + CAL_INFO_PER_FRAME * sizeof(cal_info_rec_t));
    CHECK(buf[0] == 0 && buf[1] == CAL_INFO_PER_FRAME && buf[2] == 1);
    cal_info_rec_t r;
    memcpy(&r, &buf[3], sizeof r);
    CHECK(r.status == CAL_KEY_OK && r.old_min == 1900 && r.old_max == 7900 &&
          r.old_strike == 0 && r.new_min == REST && r.new_max == REST + RANGE &&
          r.new_strike == 66 && r.n_knees == 2);
    memcpy(&r, &buf[3 + 3 * sizeof r], sizeof r);
    CHECK(r.old_strike == 70);
    len = cal_session_build_info(27, 9, buf);  // clipped to the last key
    CHECK(buf[1] == PHOTON_ACTIVE_SENSORS - 27);

    // Kept swing payload, in chunks.
    const cal_key_t *k2 = cal_session_key(2);
    len = cal_session_build_swing(2, 0, buf);
    cal_swing_hdr_t h;
    memcpy(&h, buf, sizeof h);
    CHECK(h.key == 2 && h.flags == (CAL_SWING_VALID | CAL_SWING_KNEE) && h.len == k2->kept_len);
    CHECK(h.n == CAL_SWING_PER_FRAME && len == sizeof h + 2 * h.n);
    CHECK(h.period_us == PERIOD && h.pre == PHOTON_CAL_PRE_SAMPLES);
    CHECK(h.rest >= REST && h.rest <= REST + 10);  // the capture's baseline
    CHECK(h.knee_value == REST + 63 * RANGE / 100);
    uint16_t v;
    memcpy(&v, buf + sizeof h, 2);
    CHECK(v == cal_session_swing(2)[0]);
    len = cal_session_build_swing(2, (uint16_t)(k2->kept_len - 3), buf);
    memcpy(&h, buf, sizeof h);
    CHECK(h.n == 3);
    len = cal_session_build_swing(2, k2->kept_len, buf);
    memcpy(&h, buf, sizeof h);
    CHECK(h.n == 0);
    len = cal_session_build_swing(1, 0, buf);  // nothing captured
    memcpy(&h, buf, sizeof h);
    CHECK(h.flags == 0 && h.n == 0 && h.status == CAL_KEY_UNSEEN);

    // Save: knees become thresholds, keys without one go global (0).
    CHECK(cal_session_commit());
    CHECK(g_config.strike_pct[0] == 66);
    CHECK(g_config.strike_pct[2] == 66);
    CHECK(g_config.strike_pct[3] == 66);
    CHECK(g_config.strike_pct[1] == 0 && g_config.strike_pct[4] == 0);
    CHECK(last_cmd.op == PHOTON_CMD_SET_STRIKES);
    CHECK(memcmp(g_strike_stage, g_config.strike_pct, PHOTON_MAX_SENSORS) == 0);
    // After the save the payloads report the stored values.
    len = cal_session_build_info(0, 1, buf);
    memcpy(&r, &buf[3], sizeof r);
    CHECK(r.new_strike == 66 && r.old_strike == 0);
}

static void test_long_swing_fits(void) {
    // 1.2 s approach + 1 s creep + snap + held: ~3 s, inside the 3.4 s buffer.
    static const seg_t very_slow[] = {
        { 1200, 55 }, { 1000, 63 }, { 8, 97 }, { 600, 97 }, { 150, 0 },
    };
    setup();
    play(1u << 0, very_slow, 5);
    CHECK(cal_session_status(0) == CAL_KEY_OK);
    CHECK(!(cal_session_key(0)->kept_flags & CAL_SWING_TRUNCATED));
    // Captured from early in the approach, not from the snap: the idle
    // baseline must not follow a key that is merely going down slowly. The
    // swing's rest is that baseline, not the (already moving) pre-roll.
    CHECK(cal_session_swing(0)[0] < REST + RANGE / 20);
    CHECK(cal_session_key(0)->kept_len > (1200 + 1000) / 1.667);
    CHECK(cal_session_key(0)->kept_rest >= REST && cal_session_key(0)->kept_rest <= REST + 10);
    CHECK(cal_session_strike(0, REST, REST + RANGE) == 66);
}

static void test_global_mode(void) {
    setup();
    g_config.strike_mode = PHOTON_STRIKE_MODE_GLOBAL;
    // Any full press is done: a fast stroke, and neighbours together.
    play(1u << 2, fast, 3);
    CHECK(cal_session_status(2) == CAL_KEY_OK);
    CHECK(cal_session_key(2)->n_knees == 0);
    play((1u << 3) | (1u << 4), slow, 5);
    CHECK(cal_session_status(3) == CAL_KEY_OK && cal_session_status(4) == CAL_KEY_OK);
    play(1u << 0, slow, 5);
    CHECK(cal_session_key(0)->n_knees == 1);  // knees still found, for 'strike knee'
    uint8_t buf[PHOTON_FRAME_MAX_PAYLOAD];
    cal_session_build_status(buf);
    CHECK(buf[0] == (CAL_FLAG_LEARNING | CAL_FLAG_GLOBAL));
    CHECK(buf[2 + PHOTON_ACTIVE_SENSORS + 0] == 0);  // the save applies the global threshold
    // Save: the knee table is stored, core 1 gets every key global.
    CHECK(cal_session_commit());
    CHECK(g_config.strike_pct[0] == 66 && g_config.strike_pct[2] == 0);
    uint8_t zeros[PHOTON_MAX_SENSORS] = { 0 };
    CHECK(memcmp(g_strike_stage, zeros, sizeof zeros) == 0);
    // Back to knee mode: the stored table goes to core 1, no recalibration.
    g_config.strike_mode = PHOTON_STRIKE_MODE_KNEE;
    cal_session_apply_mode();
    CHECK(last_cmd.op == PHOTON_CMD_SET_STRIKES && g_strike_stage[0] == 66);
}

static void test_rules_reanalyse(void) {
    // A key that plucks early, at 35% of its travel: below the default window.
    static const seg_t early[] = { { 300, 30 }, { 300, 35 }, { 8, 97 }, { 300, 97 }, { 150, 0 } };
    setup();
    play(1u << 0, slow, 5);
    play(1u << 2, fast, 3);
    play(1u << 4, early, 5);
    CHECK(cal_session_status(0) == CAL_KEY_OK && cal_session_status(2) == CAL_KEY_NO_KNEE);
    CHECK(cal_session_status(4) == CAL_KEY_NO_KNEE);
    // Widen the window to 30-90%: judged again, one key per pass of the loop.
    uint8_t r[4] = { 30, 25, 8, 3 };
    CHECK(cal_rules_valid(r));
    g_config.knee_window_pct = 30;
    cal_session_reanalyse();
    CHECK(cal_session_status(4) == CAL_KEY_NO_KNEE);  // not yet: the loop does it
    hold(5);
    CHECK(cal_session_status(4) == CAL_KEY_OK);
    CHECK(cal_session_strike(4, REST, REST + RANGE) == 38);
    CHECK(cal_session_status(2) == CAL_KEY_NO_KNEE);  // a fast stroke still has none
    // Stricter rules never turn a green key back (its knees may come from
    // presses the buffer no longer holds); the kept swing shows no knee.
    g_config.knee_window_pct = 0;
    g_config.knee_jump_pct = 50;
    cal_session_reanalyse();
    hold(5);
    CHECK(cal_session_status(0) == CAL_KEY_OK && cal_session_key(0)->n_knees == 1);
    CHECK(!(cal_session_key(0)->kept_flags & CAL_SWING_KNEE));
    // Back to the default: the kept swing's knee again.
    g_config.knee_jump_pct = 0;
    cal_session_reanalyse();
    hold(5);
    CHECK(cal_session_status(0) == CAL_KEY_OK && cal_session_key(0)->n_knees == 1);
    CHECK(cal_session_key(0)->kept_flags & CAL_SWING_KNEE);
    // A wider margin moves the threshold.
    g_config.knee_margin_pct = 6;
    CHECK(cal_session_strike(0, REST, REST + RANGE) == 69);
    g_config.knee_margin_pct = 0;
    // Out-of-range rules are refused.
    uint8_t bad1[4] = { 41, 25, 8, 3 }, bad2[4] = { 15, 5, 8, 3 }, bad3[4] = { 15, 25, 8, 21 };
    CHECK(!cal_rules_valid(bad1) && !cal_rules_valid(bad2) && !cal_rules_valid(bad3));
}

static void test_commit_without_session_is_noop(void) {
    setup();
    g_events.learning = false;  // frozen: 'cal save' reaching a board not calibrating
    hold(2);
    CHECK(!cal_session_active());
    g_config.strike_pct[0] = 71;
    int before = cmds_pushed;
    CHECK(!cal_session_commit());
    CHECK(g_config.strike_pct[0] == 71);
    CHECK(cmds_pushed == before);
}

static void test_strike_clamped_to_window(void) {
    setup();
    play(1u << 0, slow, 5);
    // Against a much deeper calibrated range the knee reads shallow: the
    // threshold still stays inside the search window.
    CHECK(cal_session_strike(0, REST, REST + 3 * RANGE) ==
          PHOTON_STRIKE_PCT - PHOTON_KNEE_WINDOW_PCT);
    CHECK(cal_session_strike(0, REST, REST + RANGE / 2) ==
          PHOTON_STRIKE_PCT + PHOTON_KNEE_WINDOW_PCT + PHOTON_KNEE_MARGIN_PCT);
    CHECK(cal_session_strike(0, 5000, 5000) == 0);  // degenerate range
}

int main(void) {
    test_session();
    test_long_swing_fits();
    test_global_mode();
    test_rules_reanalyse();
    test_commit_without_session_is_noop();
    test_strike_clamped_to_window();
    printf("test_cal_session OK (%d checks)\n", checks);
    return 0;
}
