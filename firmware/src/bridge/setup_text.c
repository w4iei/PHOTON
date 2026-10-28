#include "bridge/setup_text.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char CUT[] = "... (cut here: the text is longer than the buffer)\n";

typedef struct {
    char *out;
    size_t cap;
    size_t len;
    bool cut;
} writer_t;

// One whole line or nothing, always leaving room for the cut marker.
__attribute__((format(printf, 2, 3)))
static void line(writer_t *w, const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (w->cut || n < 0) {
        return;
    }
    if ((size_t)n >= sizeof buf) {
        n = (int)sizeof buf - 1;
    }
    if (w->len + (size_t)n + 1 + sizeof CUT > w->cap) {
        w->cut = true;
        if (w->len + sizeof CUT <= w->cap) {
            memcpy(w->out + w->len, CUT, sizeof CUT);  // with its NUL
            w->len += sizeof CUT - 1;
        }
        return;
    }
    memcpy(w->out + w->len, buf, (size_t)n);
    w->len += (size_t)n;
    w->out[w->len++] = '\n';
    w->out[w->len] = '\0';
}

static const char *note_str(int16_t note, char *buf, size_t n) {
    static const char *names[12] = { "C", "C#", "D", "D#", "E", "F",
                                     "F#", "G", "G#", "A", "A#", "B" };
    if (note < 0 || note > 127) {
        snprintf(buf, n, "-");
    } else {
        snprintf(buf, n, "%s%d", names[note % 12], note / 12 - 1);
    }
    return buf;
}

static void board(writer_t *w, uint8_t id, const setup_node_t *nd) {
    if (nd->state == SETUP_NODE_NO_REPLY) {
        line(w, "== board %u: on the bus, but its calibration did not arrive ==", id);
        line(w, "%s", "");
        return;
    }
    bool global = (nd->flags & CAL_FLAG_GLOBAL) != 0;
    line(w, "== board %u: strike %s%s ==", id,
         global ? "global, every key at the fixed threshold" : "knee, per-key thresholds",
         (nd->flags & CAL_FLAG_LEARNING) ? " (calibrating: values as a save would store them)"
                                         : "");
    line(w, "idx note   min   max thr%%    thr");
    for (uint8_t i = 0; i < PHOTON_ACTIVE_SENSORS; i++) {
        const cal_info_rec_t *r = &nd->key[i];
        char nm[8];
        note_str(nd->note[i], nm, sizeof nm);
        if (r->status == CAL_KEY_DISABLED) {
            line(w, "%3u %-4s disabled", i, nm);
            continue;
        }
        unsigned pct = r->new_strike ? r->new_strike : PHOTON_STRIKE_PCT;
        char g = r->new_strike ? ' ' : 'g';
        if (r->new_min == 0xFFFF || r->new_max <= r->new_min) {
            line(w, "%3u %-4s     -     - %3u%c      -", i, nm, pct, g);
            continue;
        }
        long thr = r->new_min + (long)(r->new_max - r->new_min) * (long)pct / 100;
        line(w, "%3u %-4s %5u %5u %3u%c %6ld", i, nm, r->new_min, r->new_max, pct, g, thr);
    }
    line(w, "%s", "");
}

size_t setup_text_format(char *out, size_t cap, const setup_info_t *s) {
    writer_t w = { out, cap, 0, false };
    if (cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (cap <= sizeof CUT) {
        return 0;
    }
    uint32_t t = s->read_ms / 1000u;
    line(&w, "PHOTON setup for the recordings in this directory");
    line(&w, "Read at power-on +%02lu:%02lu:%02lu; a change made later shows in the next"
             " power-on's SETUP.TXT.",
         (unsigned long)(t / 3600u), (unsigned long)(t / 60u % 60u), (unsigned long)(t % 60u));
    line(&w, "%s", "");
    line(&w, "bridge      fw %s (%s), hw %s", s->build_id, s->build_date, s->serial);
    for (uint32_t m = 0; m < PHOTON_MAX_MANUALS; m++) {
        line(&w, "%-11s manual %lu (boards %lu-%lu) on channel %u%s", m == 0 ? "channels" : "",
             (unsigned long)(m + 1), (unsigned long)(m * 2 + 1), (unsigned long)(m * 2 + 2),
             s->channel[m], s->channel_auto[m] ? " (auto)" : "");
    }
    line(&w, "notes       %u-%u on each manual", s->midi_low, s->midi_high);
    line(&w, "velocity    velrange %.0f-%.0f, velcurve %.1f %.1f %.2f", (double)s->vel_out_min,
         (double)s->vel_out_max, (double)s->vel_min_ms, (double)s->vel_max_ms,
         (double)s->vel_curve);
    line(&w, "strike      %s (the bridge's copy; each board's own is in its table)",
         s->strike_global ? "global" : "knee");
    line(&w, "knee rules  %u-%u%% of the travel, %u.%ux, %u%%, +%u%% (the bridge's copy;"
             " used only while calibrating)",
         s->rules.lo_pct, s->rules.hi_pct, s->rules.ratio_x10 / 10, s->rules.ratio_x10 % 10,
         s->rules.jump_pct, s->margin_pct);
    line(&w, "%s", "");
    line(&w, "Per key: min/max = calibrated range, thr = strike threshold in %% of it"
             " (g = the global %d%%) and in counts.", PHOTON_STRIKE_PCT);
    line(&w, "%s", "");

    char absent[40] = "";
    size_t alen = 0;
    for (uint8_t id = 1; id <= PHOTON_MAX_NODE_ID; id++) {
        const setup_node_t *nd = &s->node[id];
        if (nd->state == SETUP_NODE_ABSENT) {
            alen += (size_t)snprintf(absent + alen, sizeof absent - alen, " %u", id);
            continue;
        }
        board(&w, id, nd);
    }
    if (alen) {
        line(&w, "boards not on the bus:%s", absent);
    }
    return w.len;
}
