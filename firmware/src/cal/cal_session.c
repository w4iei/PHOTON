#include "cal/cal_session.h"

#include <stdint.h>
#include <string.h>

#include "hardware/sync.h"

#include "cal/knee.h"
#include "config/config_store.h"
#include "core1/calcap.h"
#include "core1/events.h"
#include "ipc/rings.h"

#define MIN_RANGE ((uint32_t)PHOTON_MIN_EVENT_RANGE << PHOTON_OSR_MODE)
// A capture counts as a key press only if it spans at least this share of
// the key's calibrated range: a neighbour's cross-illumination moves a
// sensor well past its noise floor, never through its whole travel.
#define FULL_PRESS_PCT 50

static struct {
    uint32_t session;   // g_calcap.session last followed
    int next;           // round-robin start for the next analysis
    uint32_t rejudge;   // keys whose kept swing waits to be judged again
    bool have;
    bool active;
    bool committed;
    cal_key_t key[PHOTON_ACTIVE_SENSORS];
} S;

static uint16_t kept[PHOTON_ACTIVE_SENSORS][PHOTON_CAL_CAP_SAMPLES];

static inline bool disabled(int i) {
    return (g_events.disabled_mask >> i) & 1u;
}

static inline bool inverted(int i) {
    return (g_events.polarity_mask >> i) & 1u;
}

knee_rules_t cal_session_rules(uint8_t *margin_pct) {
    uint32_t w = g_config.knee_window_pct ? g_config.knee_window_pct : PHOTON_KNEE_WINDOW_PCT;
    knee_rules_t r;
    r.lo_pct = (uint8_t)(w < PHOTON_STRIKE_PCT ? PHOTON_STRIKE_PCT - w : 1);
    r.hi_pct = (uint8_t)(PHOTON_STRIKE_PCT + w < 100 ? PHOTON_STRIKE_PCT + w : 99);
    r.ratio_x10 = g_config.knee_ratio_x10 ? g_config.knee_ratio_x10 : PHOTON_KNEE_RATIO_X10;
    r.jump_pct = g_config.knee_jump_pct ? g_config.knee_jump_pct : PHOTON_KNEE_JUMP_PCT;
    if (margin_pct) {
        *margin_pct = g_config.knee_margin_pct ? g_config.knee_margin_pct
                                               : PHOTON_KNEE_MARGIN_PCT;
    }
    return r;
}

bool cal_rules_valid(const uint8_t *r) {
    return r[0] <= 40 && (r[1] == 0 || (r[1] >= 11 && r[1] <= 100)) && r[2] <= 50 &&
           r[3] <= 20;
}

bool cal_session_global(void) {
    return g_config.strike_mode == PHOTON_STRIKE_MODE_GLOBAL;
}

// A key's status from one full swing. Global mode asks for nothing more
// than the full press; knee mode for a knee, pressed alone.
static void judge(cal_key_t *k, const knee_result_t *r, bool adj) {
    if (r->reason == KNEE_OK && !adj) {
        k->knees[k->knee_head] = r->value;
        k->knee_head = (uint8_t)((k->knee_head + 1) % PHOTON_CAL_MAX_KNEES);
        if (k->n_knees < PHOTON_CAL_MAX_KNEES) {
            k->n_knees++;
        }
    }
    if (cal_session_global() || (r->reason == KNEE_OK && !adj)) {
        k->status = CAL_KEY_OK;
    } else if (k->status != CAL_KEY_OK) {
        k->status = adj ? CAL_KEY_ADJACENT : CAL_KEY_NO_KNEE;
    }
}

static void keep_result(cal_key_t *k, const knee_result_t *r) {
    bool ok = r->reason == KNEE_OK;
    k->kept_flags = (uint8_t)((k->kept_flags & ~CAL_SWING_KNEE) | (ok ? CAL_SWING_KNEE : 0));
    k->kept_pct = ok ? r->pct : 0;
    k->kept_knee_idx = ok ? r->idx : 0;
    k->kept_knee_value = ok ? r->value : 0;
    k->kept_rest = r->rest;
    k->kept_top = r->top;
}

static void begin(void) {
    S.have = true;
    S.committed = false;
    S.rejudge = 0;
    for (int i = 0; i < PHOTON_ACTIVE_SENSORS; i++) {
        cal_key_t *k = &S.key[i];
        memset(k, 0, sizeof *k);
        k->status = CAL_KEY_UNSEEN;
        k->old_min = g_config.cal_min[i];
        k->old_max = g_config.cal_max[i];
        k->old_strike = g_config.strike_pct[i];
    }
}

// Did either neighbour go down while this key was down? Only a real press
// counts (at least half this key's depth), not the neighbour's crosstalk.
static bool neighbour_pressed(int i, const calcap_key_t *ck) {
    for (int d = -1; d <= 1; d += 2) {
        int j = i + d;
        if (j < 0 || j >= PHOTON_ACTIVE_SENSORS || disabled(j)) {
            continue;
        }
        const calcap_key_t *nk = &g_calcap.key[j];
        uint8_t st = nk->state;
        if (nk->swings == 0 && st != CALCAP_ACTIVE) {
            continue;
        }
        bool started_before_end = (int32_t)(nk->t_start_us - ck->t_end_us) < 0;
        bool overlaps = st == CALCAP_ACTIVE ||
                        (int32_t)(nk->t_end_us - ck->t_start_us) > 0;
        if (started_before_end && overlaps && 2u * nk->amp >= ck->amp) {
            return true;
        }
    }
    return false;
}

static void analyse(int i, const uint16_t *live_min, const uint16_t *live_max) {
    calcap_key_t *ck = &g_calcap.key[i];
    __dmb();
    uint32_t period = g_calcap.period_us ? g_calcap.period_us : 1000000u / PHOTON_DEFAULT_SCAN_RATE_HZ;
    knee_rules_t rules = cal_session_rules(NULL);
    knee_result_t r = knee_find(ck->samples, ck->len, ck->pre_len, ck->swing_rest, period,
                                inverted(i), &rules, MIN_RANGE);
    uint32_t amp = r.top > r.rest ? (uint32_t)(r.top - r.rest) : (uint32_t)(r.rest - r.top);
    uint32_t cal_rng = live_max[i] > live_min[i] ? (uint32_t)(live_max[i] - live_min[i]) : 0;
    bool full = r.reason != KNEE_SHALLOW && amp * 100u >= cal_rng * FULL_PRESS_PCT;
    if (full) {
        cal_key_t *k = &S.key[i];
        bool adj = neighbour_pressed(i, ck);
        k->swings++;
        judge(k, &r, adj);
        uint16_t len = ck->len;
        memcpy(kept[i], ck->samples, (size_t)len * sizeof kept[i][0]);
        k->kept_len = len;
        k->kept_pre = ck->pre_len;
        k->kept_flags = (uint8_t)(CAL_SWING_VALID |
                                  (ck->truncated ? CAL_SWING_TRUNCATED : 0) |
                                  (adj ? CAL_SWING_ADJACENT : 0));
        keep_result(k, &r);
        k->kept_period_us = (uint16_t)(period > 0xFFFF ? 0xFFFF : period);
    }
    __dmb();
    ck->state = CALCAP_IDLE;  // buffer back to core 1
}

static void rejudge(int i);

void cal_session_task(void) {
    uint32_t sess = g_calcap.session;
    if (sess != S.session) {
        S.session = sess;
        begin();
    }
    S.active = S.have && g_events.learning;
    if (!S.active) {
        return;
    }
    if (S.rejudge != 0) {
        // 'cal rules': one key per call, like the swings below.
        int i = __builtin_ctz(S.rejudge);
        S.rejudge &= ~(1u << i);
        rejudge(i);
        return;
    }
    // One swing per call: core 0 also answers the bus, and the main loop
    // comes straight back for the next one.
    for (int n = 0; n < PHOTON_ACTIVE_SENSORS; n++) {
        int i = (S.next + n) % PHOTON_ACTIVE_SENSORS;
        if (g_calcap.key[i].state == CALCAP_DONE) {
            photon_snapshot_t snap;
            snapshot_read(&snap);
            analyse(i, snap.min, snap.max);
            S.next = (i + 1) % PHOTON_ACTIVE_SENSORS;
            return;
        }
    }
}

bool cal_session_active(void) { return S.active; }
bool cal_session_have(void) { return S.have; }

uint8_t cal_session_status(uint8_t idx) {
    if (idx >= PHOTON_ACTIVE_SENSORS || disabled(idx)) {
        return CAL_KEY_DISABLED;
    }
    return S.have ? S.key[idx].status : CAL_KEY_UNSEEN;
}

const cal_key_t *cal_session_key(uint8_t idx) {
    return idx < PHOTON_ACTIVE_SENSORS ? &S.key[idx] : NULL;
}

const uint16_t *cal_session_swing(uint8_t idx) {
    return idx < PHOTON_ACTIVE_SENSORS ? kept[idx] : NULL;
}

uint8_t cal_session_strike(uint8_t idx, uint16_t mn, uint16_t mx) {
    if (idx >= PHOTON_ACTIVE_SENSORS || !S.have || mx <= mn) {
        return 0;
    }
    const cal_key_t *k = &S.key[idx];
    if (k->status != CAL_KEY_OK || k->n_knees == 0) {
        return 0;
    }
    // Median of the kept knees (insertion sort; at most PHOTON_CAL_MAX_KNEES).
    uint16_t v[PHOTON_CAL_MAX_KNEES];
    uint8_t n = k->n_knees;
    memcpy(v, k->knees, (size_t)n * sizeof v[0]);
    for (uint8_t a = 1; a < n; a++) {
        uint16_t x = v[a];
        int b = a - 1;
        while (b >= 0 && v[b] > x) {
            v[b + 1] = v[b];
            b--;
        }
        v[b + 1] = x;
    }
    uint32_t knee = n % 2 ? v[n / 2] : ((uint32_t)v[n / 2 - 1] + v[n / 2]) / 2u;
    uint32_t rng = (uint32_t)(mx - mn);
    uint8_t margin;
    knee_rules_t rules = cal_session_rules(&margin);
    int32_t depth = inverted(idx) ? (int32_t)mx - (int32_t)knee : (int32_t)knee - (int32_t)mn;
    int32_t pct = (depth * 100 + (int32_t)rng / 2) / (int32_t)rng + margin;
    if (pct < rules.lo_pct) {
        pct = rules.lo_pct;
    }
    if (pct > rules.hi_pct + margin) {
        pct = rules.hi_pct + margin;
    }
    return (uint8_t)(pct > 99 ? 99 : pct);
}

bool cal_session_commit(void) {
    if (!S.active) {
        return false;
    }
    photon_snapshot_t snap;
    snapshot_read(&snap);
    memset(g_config.strike_pct, 0, sizeof g_config.strike_pct);
    for (uint8_t i = 0; i < PHOTON_ACTIVE_SENSORS; i++) {
        g_config.strike_pct[i] = cal_session_strike(i, snap.min[i], snap.max[i]);
    }
    cal_session_apply_mode();
    S.committed = true;
    return true;
}

void cal_session_apply_mode(void) {
    if (cal_session_global()) {
        memset(g_strike_stage, 0, sizeof g_strike_stage);
    } else {
        memcpy(g_strike_stage, g_config.strike_pct, sizeof g_strike_stage);
    }
    photon_cmd_t c = { .op = PHOTON_CMD_SET_STRIKES };
    cmd_mailbox_push(&c);  // the push's barrier publishes the stage
}

void cal_session_reanalyse(void) {
    if (!S.active) {
        return;
    }
    for (int i = 0; i < PHOTON_ACTIVE_SENSORS; i++) {
        if (S.key[i].kept_flags & CAL_SWING_VALID) {
            S.rejudge |= 1u << i;
        }
    }
}

// Judge a key's kept swing again under the rules in force. A knee found
// replaces the key's knees; none found leaves a green key green (its knees
// came from other presses the buffer no longer holds).
static void rejudge(int i) {
    cal_key_t *k = &S.key[i];
    knee_rules_t rules = cal_session_rules(NULL);
    knee_result_t r = knee_find(kept[i], k->kept_len, k->kept_pre, k->kept_rest,
                                k->kept_period_us, inverted(i), &rules, MIN_RANGE);
    bool adj = (k->kept_flags & CAL_SWING_ADJACENT) != 0;
    if (r.reason == KNEE_OK && !adj) {
        k->n_knees = 0;
        k->knee_head = 0;
        judge(k, &r, false);
    } else if (k->status != CAL_KEY_OK) {
        judge(k, &r, adj);
    }
    keep_result(k, &r);
}

// ---------------------------------------------------------------------------
// Wire payloads
// ---------------------------------------------------------------------------

static uint8_t new_strike(uint8_t i, const photon_snapshot_t *snap) {
    if (cal_session_global()) {
        return 0;
    }
    return S.active && !S.committed ? cal_session_strike(i, snap->min[i], snap->max[i])
                                    : g_config.strike_pct[i];
}

static uint8_t flags(void) {
    return (uint8_t)((S.active ? CAL_FLAG_LEARNING : 0) |
                     (cal_session_global() ? CAL_FLAG_GLOBAL : 0));
}

uint8_t cal_session_build_status(uint8_t *out) {
    photon_snapshot_t snap;
    snapshot_read(&snap);
    const uint8_t n = PHOTON_ACTIVE_SENSORS;
    out[0] = flags();
    out[1] = n;
    for (uint8_t i = 0; i < n; i++) {
        out[2 + i] = cal_session_status(i);
        out[2 + n + i] = new_strike(i, &snap);
    }
    return (uint8_t)(2 + 2 * n);
}

uint8_t cal_session_build_info(uint8_t start, uint8_t count, uint8_t *out) {
    if (start >= PHOTON_ACTIVE_SENSORS) {
        start = 0;
        count = 0;
    }
    if (count > PHOTON_ACTIVE_SENSORS - start) {
        count = (uint8_t)(PHOTON_ACTIVE_SENSORS - start);
    }
    if (count > CAL_INFO_PER_FRAME) {
        count = CAL_INFO_PER_FRAME;
    }
    photon_snapshot_t snap;
    snapshot_read(&snap);
    out[0] = start;
    out[1] = count;
    out[2] = flags();
    for (uint8_t c = 0; c < count; c++) {
        uint8_t i = (uint8_t)(start + c);
        cal_info_rec_t rec;
        rec.status = cal_session_status(i);
        rec.new_strike = new_strike(i, &snap);
        rec.old_strike = S.have ? S.key[i].old_strike : g_config.strike_pct[i];
        rec.n_knees = S.have ? S.key[i].n_knees : 0;
        rec.old_min = S.have ? S.key[i].old_min : g_config.cal_min[i];
        rec.old_max = S.have ? S.key[i].old_max : g_config.cal_max[i];
        rec.new_min = snap.min[i];
        rec.new_max = snap.max[i];
        memcpy(&out[3 + c * sizeof rec], &rec, sizeof rec);
    }
    return (uint8_t)(3 + count * sizeof(cal_info_rec_t));
}

uint8_t cal_session_build_swing(uint8_t key, uint16_t offset, uint8_t *out) {
    cal_swing_hdr_t h;
    memset(&h, 0, sizeof h);
    h.key = key;
    h.offset = offset;
    if (key < PHOTON_ACTIVE_SENSORS && S.have) {
        const cal_key_t *k = &S.key[key];
        h.flags = k->kept_flags;
        h.len = k->kept_len;
        h.pre = k->kept_pre;
        h.period_us = k->kept_period_us;
        h.knee_idx = k->kept_knee_idx;
        h.knee_value = k->kept_knee_value;
        h.rest = k->kept_rest;
        h.top = k->kept_top;
        h.status = cal_session_status(key);
        if ((k->kept_flags & CAL_SWING_VALID) && offset < k->kept_len) {
            uint16_t n = (uint16_t)(k->kept_len - offset);
            if (n > CAL_SWING_PER_FRAME) {
                n = CAL_SWING_PER_FRAME;
            }
            h.n = (uint8_t)n;
            memcpy(out + sizeof h, &kept[key][offset], (size_t)n * 2);
        }
    }
    memcpy(out, &h, sizeof h);
    return (uint8_t)(sizeof h + (size_t)h.n * 2);
}
