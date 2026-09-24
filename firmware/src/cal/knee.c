#include "cal/knee.h"

// Shape of a pluck, in fractions of the swing's rest..top range per ms.
// Tuned 2026-09-24 on 34 real swings of a double's upper manual (plucks at
// 41-73% of travel; the snap builds up over 5-13 ms there, not in one
// sample) and checked against the January 2026 single-sensor traces. The
// window, the ratio and the jump come from knee_rules_t.
#define RISE_MS         8.0f    // span the post-knee slope is measured over
#define BEFORE_MS       20.0f   // span of the slow approach before it
#define SHORT_BEFORE_MS 10.0f   // ... and of the last moments before it: a
                                // finger speeding up ramps over tens of ms,
                                // a pluck builds its snap within ~10 ms
#define JUMP_WITHIN_MS  15.0f   // the jump is measured this long after the
                                // knee; half of it must still be there
                                // RISE_MS after the rise

static inline uint32_t press_of(const uint16_t *s, uint32_t i, bool inverted) {
    return inverted ? 65535u - s[i] : s[i];
}

static inline uint32_t span(float ms, float dt_ms, uint32_t floor) {
    uint32_t n = (uint32_t)(ms / dt_ms + 0.5f);
    return n < floor ? floor : n;
}

knee_result_t knee_find(const uint16_t *s, uint32_t n, uint32_t pre, uint16_t rest_hint,
                        uint32_t period_us, bool inverted, const knee_rules_t *rules,
                        uint32_t min_range) {
    knee_result_t r = { .reason = KNEE_SHALLOW };
    if (s == 0 || rules == 0 || n < 8 || period_us == 0) {
        return r;
    }
    if (pre < 1) {
        pre = 1;
    }
    if (pre > n) {
        pre = n;
    }

    // Levels in "press" orientation: larger = further down. Without a given
    // rest, the older half of the pre-roll: a slow press creeps through its
    // newest samples before it crosses the swing threshold.
    uint32_t rest;
    if (rest_hint != 0) {
        rest = inverted ? 65535u - rest_hint : rest_hint;
    } else {
        uint32_t n_rest = pre > 1 ? pre / 2 : 1;
        uint32_t acc = 0;
        for (uint32_t i = 0; i < n_rest; i++) {
            acc += press_of(s, i, inverted);
        }
        rest = acc / n_rest;
    }
    uint32_t top = 0;
    for (uint32_t i = 1; i + 1 < n; i++) {
        uint32_t m3 = (press_of(s, i - 1, inverted) + press_of(s, i, inverted) +
                       press_of(s, i + 1, inverted)) / 3u;
        if (m3 > top) {
            top = m3;
        }
    }
    r.rest = (uint16_t)(inverted ? 65535u - rest : rest);
    r.top = (uint16_t)(inverted ? 65535u - top : top);
    if (top <= rest || top - rest < min_range) {
        return r;
    }

    const float inv_range = 1.0f / (float)(top - rest);
    const float dt = (float)period_us / 1000.0f;
    const uint32_t k = span(RISE_MS, dt, 2);
    const uint32_t m = span(BEFORE_MS, dt, 3);
    const uint32_t ms = span(SHORT_BEFORE_MS, dt, 2);
    const uint32_t jw = span(JUMP_WITHIN_MS, dt, k);
    const float lo = (float)rules->lo_pct / 100.0f;
    const float hi = (float)rules->hi_pct / 100.0f;
    const float ratio = (float)rules->ratio_x10 / 10.0f;
    const float jump = (float)rules->jump_pct / 100.0f;
#define Q(i) (((float)press_of(s, (i), inverted) - (float)rest) * inv_range)

    r.reason = KNEE_NONE;
    for (uint32_t i = m + k; i < n; i++) {
        uint32_t j = i - k;  // candidate knee: the rise is measured over j..i
        float qj = Q(j);
        if (qj < lo || qj > hi) {
            continue;  // a glitch above the window must not end the search
        }
        float after = (Q(i) - qj) / ((float)k * dt);
        float before = (qj - Q(j - m)) / ((float)m * dt);
        float before_short = (qj - Q(j - ms)) / ((float)ms * dt);
        if (after <= 0.0f || after < ratio * before || after < ratio * before_short) {
            continue;
        }
        // The key stays down after a pluck; a one-sample glitch comes back.
        // Judged a little later (i+k .. i+2k), so a softer snap has had
        // time to get there.
        if (i + 2 * k >= n) {
            continue;
        }
        float held = Q(i + k);
        for (uint32_t t = i + k + 1; t <= i + 2 * k; t++) {
            float q = Q(t);
            if (q < held) {
                held = q;
            }
        }
        if (held - qj < 0.5f * jump) {
            continue;
        }
        float peak = qj;
        for (uint32_t t = j; t < n && t <= j + jw; t++) {
            float q = Q(t);
            if (q > peak) {
                peak = q;
            }
        }
        if (peak - qj < jump) {
            continue;
        }
        // Tighten to the last sample before the steep steps begin: forward
        // over slow steps, and back if j already sits inside the snap (a
        // pluck just below the window is not reported at its edge).
        float step_thr = 0.5f * after * dt;
        uint32_t knee = j;
        while (knee + 1 < i && Q(knee + 1) - Q(knee) < step_thr) {
            knee++;
        }
        while (knee > 0 && Q(knee) - Q(knee - 1) >= step_thr) {
            knee--;
        }
        if (Q(knee) < lo) {
            continue;
        }
        float pct = Q(knee) * 100.0f + 0.5f;
        r.reason = KNEE_OK;
        r.idx = (uint16_t)knee;
        r.value = s[knee];
        r.pct = (uint8_t)(pct < 0.0f ? 0.0f : pct > 100.0f ? 100.0f : pct);
        return r;
    }
#undef Q
    return r;
}
