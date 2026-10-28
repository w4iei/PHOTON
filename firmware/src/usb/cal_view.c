#include "usb/cal_view.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/time.h"

#include "board_config.h"
#include "bridge/midi_map.h"
#include "cal/cal_session.h"
#include "comms/protocol.h"
#include "config/config_store.h"
#include "util/log.h"

#define POLL_MS          250   // remote status poll while the view is up
#define DRAW_MS          250
#define STALE_MS         1500  // a node silent this long drops out of the view
#define FIRST_DRAW_MS    300   // let the first replies arrive
#define SWING_REPLY_MS   60
#define SWING_TRIES      4
#define COMPARE_GAP_MS   400   // between nodes (4 requests each)

typedef struct {
    bool learning;
    bool global;
    absolute_time_t seen;
    uint8_t status[PHOTON_MAX_SENSORS];
} node_state_t;

static struct {
    bool is_bridge;
    bool sensor_role;
    bool showing;
    bool clear_screen;
    absolute_time_t next_poll;
    absolute_time_t next_draw;
    node_state_t node[PHOTON_MAX_NODE_ID + 1];
    struct {
        uint8_t ids[PHOTON_MAX_NODE_ID];
        int n;
        int next;
        absolute_time_t at;
    } cmp;
    struct {
        bool active;
        bool local;
        uint8_t node;
        uint8_t key;
        uint8_t key_last;
        uint16_t offset;
        uint8_t tries;
        absolute_time_t deadline;
    } dump;
} V;

void calview_init(bool is_bridge, bool sensor_role) {
    memset(&V, 0, sizeof V);
    V.is_bridge = is_bridge;
    V.sensor_role = sensor_role;
}

void calview_set_bus_master(bool on) { V.is_bridge = on; }

static bool is_local(uint8_t node) {
    return V.sensor_role && node == g_config.node_id;
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

static const char *status_word(uint8_t st) {
    switch (st) {
    case CAL_KEY_OK:       return "ok";
    case CAL_KEY_NO_KNEE:  return "no_knee";
    case CAL_KEY_ADJACENT: return "adjacent";
    case CAL_KEY_COUPLED:  return "coupled";
    case CAL_KEY_DISABLED: return "disabled";
    default:               return "unseen";
    }
}

// ---------------------------------------------------------------------------
// Live keyboard view
// ---------------------------------------------------------------------------

// Status of one key; false = no data (that board is not calibrating).
static bool key_status(uint8_t node, uint8_t idx, uint8_t *st) {
    if (is_local(node)) {
        if (!cal_session_active()) {
            return false;
        }
        *st = cal_session_status(idx);
        return true;
    }
    if (!V.is_bridge || node < 1 || node > PHOTON_MAX_NODE_ID) {
        return false;
    }
    const node_state_t *ns = &V.node[node];
    if (!ns->learning ||
        absolute_time_diff_us(ns->seen, get_absolute_time()) > (int64_t)STALE_MS * 1000) {
        return false;
    }
    *st = ns->status[idx];
    return true;
}

enum { ST_PLAIN = 0, ST_DIM, ST_GREEN, ST_YELLOW, ST_RED };
static const char *const style_seq[] = {
    "\x1b[0m", "\x1b[0;2m", "\x1b[0;30;42m", "\x1b[0;30;43m", "\x1b[0;97;41m",
};

typedef struct {
    char buf[1024];
    int n;
    int style;
} line_t;
static line_t L;  // static: the core-0 stack is small

#define LINE_ROOM ((int)sizeof L.buf - 24)  // keeps room for the terminator

static void l_put(const char *s) {
    size_t k = strlen(s);
    if (L.n + (int)k <= LINE_ROOM) {
        memcpy(L.buf + L.n, s, k);
        L.n += (int)k;
    }
}

static void l_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int room = LINE_ROOM - L.n;
    if (room > 0) {
        int k = vsnprintf(L.buf + L.n, (size_t)room + 1, fmt, ap);
        L.n += k < 0 ? 0 : (k > room ? room : k);
    }
    va_end(ap);
}

static void l_style(int st) {
    if (st != L.style) {
        l_put(style_seq[st]);
        L.style = st;
    }
}

static void l_char(int st, char c) {
    l_style(st);
    if (L.n < LINE_ROOM) {
        L.buf[L.n++] = c;
    }
}

// Ends the line: reset colours, clear the rest of the terminal row.
static void l_emit(bool newline) {
    // Always fits: text stops at LINE_ROOM, 24 bytes short of the end.
    static const char tail[] = "\x1b[0m\x1b[K\r\n";
    size_t k = newline ? sizeof tail - 1 : sizeof tail - 3;
    memcpy(L.buf + L.n, tail, k);
    L.n += (int)k;
    log_cdc_write_all((const uint8_t *)L.buf, (size_t)L.n);
    L.n = 0;
    L.style = ST_PLAIN;
}

static int cell_style(uint8_t st, char *ch) {
    switch (st) {
    case CAL_KEY_OK:       *ch = '+'; return ST_GREEN;
    case CAL_KEY_NO_KNEE:  *ch = '?'; return ST_YELLOW;
    case CAL_KEY_ADJACENT: *ch = 'A'; return ST_YELLOW;
    case CAL_KEY_COUPLED:  *ch = 'C'; return ST_RED;
    case CAL_KEY_DISABLED: *ch = ' '; return ST_PLAIN;
    default:               *ch = '.'; return ST_DIM;
    }
}

static bool black_key(int note) {
    int pc = note % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

typedef struct {
    char text[200];
    int n;
    int count;
} list_t;

static void list_add(list_t *li, const char *name) {
    li->count++;
    int k = snprintf(li->text + li->n, sizeof li->text - (size_t)li->n, " %s", name);
    if (k > 0 && li->n + k < (int)sizeof li->text) {
        li->n += k;
    }
}

// One manual: title, octave labels, black keys raised above white keys,
// one column per note, then which keys to redo and why.
static bool draw_manual(uint32_t m) {
    static int8_t cell_board[128];
    static uint8_t cell_idx[128];
    uint8_t boards[2] = { (uint8_t)(m * 2 + 1), (uint8_t)(m * 2 + 2) };
    bool has[2] = { false, false };
    int low = 128, high = -1;
    memset(cell_board, -1, sizeof cell_board);
    for (int b = 0; b < 2; b++) {
        if (boards[b] > PHOTON_MAX_NODE_ID) {
            continue;
        }
        uint8_t probe;
        has[b] = key_status(boards[b], 0, &probe);
        for (uint8_t idx = 0; idx < PHOTON_MAX_SENSORS; idx++) {
            int16_t note = midi_map_note(boards[b], idx);
            if (note < 0 || note > 127) {
                continue;
            }
            cell_board[note] = (int8_t)b;
            cell_idx[note] = idx;
            if (note < low) {
                low = note;
            }
            if (note > high) {
                high = note;
            }
        }
    }
    if ((!has[0] && !has[1]) || high < low) {
        return false;
    }

    static uint8_t cell_st[128];
    static list_t slow, adj, cpl;
    memset(&slow, 0, sizeof slow);
    memset(&adj, 0, sizeof adj);
    memset(&cpl, 0, sizeof cpl);
    int total = 0, done = 0;
    char nm[8];
    for (int note = low; note <= high; note++) {
        cell_st[note] = 0xFF;  // no data
        int b = cell_board[note];
        uint8_t st;
        if (b < 0 || !key_status(boards[b], cell_idx[note], &st)) {
            continue;
        }
        if (st != CAL_KEY_DISABLED && midi_map_coupled(m, (uint8_t)note)) {
            st = CAL_KEY_COUPLED;
        }
        cell_st[note] = st;
        if (st == CAL_KEY_DISABLED) {
            continue;
        }
        total++;
        note_str((int16_t)note, nm, sizeof nm);
        switch (st) {
        case CAL_KEY_OK:       done++; break;
        case CAL_KEY_NO_KNEE:  list_add(&slow, nm); break;
        case CAL_KEY_ADJACENT: list_add(&adj, nm); break;
        case CAL_KEY_COUPLED:  list_add(&cpl, nm); break;
        default: break;
        }
    }

    bool global = false;
    for (int b = 0; b < 2; b++) {
        if (has[b]) {
            global |= is_local(boards[b]) ? cal_session_global() : V.node[boards[b]].global;
        }
    }
    l_printf("Manual %lu: boards %u-%u, MIDI ch %u%s   ", (unsigned long)(m + 1), boards[0],
             boards[1], midi_map_channel_for_manual(m) + 1,
             global ? ", strike global (any full press is done)" : "");
    l_style(done == total && total > 0 ? ST_GREEN : ST_PLAIN);
    l_printf(" %d/%d done ", done, total);
    l_style(ST_PLAIN);
    for (int b = 0; b < 2; b++) {
        if (!has[b] && boards[b] <= PHOTON_MAX_NODE_ID) {
            l_printf("  (board %u not calibrating)", boards[b]);
        }
    }
    l_emit(true);

    // Octave labels over each C.
    char labels[132];
    int width = high - low + 1;
    memset(labels, ' ', sizeof labels);
    for (int note = low; note <= high; note++) {
        if (note % 12 == 0) {
            note_str((int16_t)note, nm, sizeof nm);
            int col = note - low;
            for (int c = 0; nm[c] && col + c < width; c++) {
                labels[col + c] = nm[c];
            }
        }
    }
    labels[width] = 0;
    l_printf("  %s", labels);
    l_emit(true);

    for (int row = 0; row < 2; row++) {  // 0: black keys, 1: white keys
        l_put("  ");
        for (int note = low; note <= high; note++) {
            bool on_row = black_key(note) == (row == 0);
            if (!on_row || cell_st[note] == 0xFF) {
                l_char(ST_PLAIN, ' ');
                continue;
            }
            char ch;
            int style = cell_style(cell_st[note], &ch);
            l_char(style, ch);
        }
        l_emit(true);
    }
    if (slow.count) {
        l_style(ST_YELLOW);
        l_printf(" ? ");
        l_style(ST_PLAIN);
        l_printf(" no pluck seen, press slower:%s", slow.text);
        l_emit(true);
    }
    if (adj.count) {
        l_style(ST_YELLOW);
        l_printf(" A ");
        l_style(ST_PLAIN);
        l_printf(" a neighbour went down too, redo alone:%s", adj.text);
        l_emit(true);
    }
    if (cpl.count) {
        l_style(ST_RED);
        l_printf(" C ");
        l_style(ST_PLAIN);
        l_printf(" moved with the other manual, disengage the coupler and redo:%s", cpl.text);
        l_emit(true);
    }
    l_emit(true);
    return true;
}

static void draw(const char *typed, size_t typed_len) {
    l_put(V.clear_screen ? "\x1b[0m\x1b[2J\x1b[H" : "\x1b[H");
    V.clear_screen = false;
    l_printf("PHOTON calibration: slow-press each key until it plucks, then let it up.");
    l_emit(true);
    l_printf("Coupler off. Keys that are not neighbours may go down together.");
    l_emit(true);
    l_char(ST_GREEN, '+');
    l_style(ST_PLAIN);
    l_printf(" done  ");
    l_char(ST_DIM, '.');
    l_style(ST_PLAIN);
    l_printf(" not yet  ");
    l_char(ST_YELLOW, '?');
    l_style(ST_PLAIN);
    l_printf(" press slower  ");
    l_char(ST_YELLOW, 'A');
    l_style(ST_PLAIN);
    l_printf(" redo alone  ");
    l_char(ST_RED, 'C');
    l_style(ST_PLAIN);
    l_printf(" coupler engaged");
    l_emit(true);
    l_emit(true);

    bool any = false;
    for (uint32_t m = 0; m < PHOTON_MAX_MANUALS; m++) {
        any |= draw_manual(m);
    }
    if (!any) {
        l_printf("Waiting for a board in calibration ('cal reset <id>' starts one).");
        l_emit(true);
        l_emit(true);
    }
    l_printf("'cal save [id ...]' stores it (and shows before/after).");
    l_emit(true);
    l_put("> ");
    if (typed_len > 0 && typed_len < 200) {
        memcpy(L.buf + L.n, typed, typed_len);
        L.n += (int)typed_len;
    }
    l_put("\x1b[J");  // clear anything below: the prompt is the last line
    log_cdc_write_all((const uint8_t *)L.buf, (size_t)L.n);
    L.n = 0;
    L.style = ST_PLAIN;
}

void calview_show(bool on, bool fresh) {
    if (on) {
        if (fresh) {
            memset(V.node, 0, sizeof V.node);
            midi_map_clear_coupled();
        }
        V.showing = true;
        V.clear_screen = true;
        V.next_poll = get_absolute_time();
        V.next_draw = make_timeout_time_ms(FIRST_DRAW_MS);
    } else if (V.showing) {
        V.showing = false;
        log_printf("\x1b[0m");
    }
}

bool calview_showing(void) { return V.showing; }

bool calview_busy(void) { return V.showing || V.dump.active; }

// ---------------------------------------------------------------------------
// Before / after
// ---------------------------------------------------------------------------

static void fmt_u(char *buf, size_t n, long v) {
    if (v < 0) {
        snprintf(buf, n, "-");
    } else {
        snprintf(buf, n, "%ld", v);
    }
}

static void print_info_payload(uint8_t node, const uint8_t *p, uint8_t len) {
    if (len < 3) {
        return;
    }
    uint8_t start = p[0];
    uint8_t count = p[1];
    uint8_t fit = (uint8_t)((len - 3) / sizeof(cal_info_rec_t));
    if (count > fit) {
        count = fit;
    }
    if (start == 0) {
        log_printf("== board %u: calibration before -> after%s%s ==", node,
                   (p[2] & CAL_FLAG_LEARNING)
                       ? " (still calibrating: 'after' is what a save would store now)"
                       : "",
                   (p[2] & CAL_FLAG_GLOBAL) ? " (strike global: every key at the fixed"
                                              " threshold, knees kept)" : "");
        log_printf("  thr = strike threshold, %% of the min..max range (g = global %d%%) and"
                   " in counts; knees = plucks captured", PHOTON_STRIKE_PCT);
        log_printf("idx note |   min   max thr%%   thr ->   min   max thr%%   thr | knees  d_thr"
                   " status");
    }
    for (uint8_t c = 0; c < count; c++) {
        cal_info_rec_t r;
        memcpy(&r, &p[3 + c * sizeof r], sizeof r);
        uint8_t idx = (uint8_t)(start + c);
        char nm[8];
        note_str(midi_map_note(node, idx), nm, sizeof nm);
        if (r.status == CAL_KEY_DISABLED) {
            log_printf("%3u %-4s | disabled", idx, nm);
            continue;
        }
        bool old_ok = r.old_min != 0xFFFF && r.old_max > r.old_min;
        bool new_ok = r.new_min != 0xFFFF && r.new_max > r.new_min;
        unsigned op = r.old_strike ? r.old_strike : PHOTON_STRIKE_PCT;
        unsigned np = r.new_strike ? r.new_strike : PHOTON_STRIKE_PCT;
        long othr = old_ok ? r.old_min + (long)(r.old_max - r.old_min) * (long)op / 100 : -1;
        long nthr = new_ok ? r.new_min + (long)(r.new_max - r.new_min) * (long)np / 100 : -1;
        char omin[8], omax[8], ot[8], nmin[8], nmax[8], nt[8], d[10];
        fmt_u(omin, sizeof omin, old_ok ? r.old_min : -1);
        fmt_u(omax, sizeof omax, old_ok ? r.old_max : -1);
        fmt_u(ot, sizeof ot, othr);
        fmt_u(nmin, sizeof nmin, new_ok ? r.new_min : -1);
        fmt_u(nmax, sizeof nmax, new_ok ? r.new_max : -1);
        fmt_u(nt, sizeof nt, nthr);
        if (old_ok && new_ok) {
            snprintf(d, sizeof d, "%+ld", nthr - othr);
        } else {
            snprintf(d, sizeof d, "-");
        }
        log_printf("%3u %-4s | %5s %5s %3u%c %5s -> %5s %5s %3u%c %5s | %5u %6s %s", idx, nm,
                   omin, omax, op, r.old_strike ? ' ' : 'g', ot, nmin, nmax, np,
                   r.new_strike ? ' ' : 'g', nt, r.n_knees, d, status_word(r.status));
    }
    if ((unsigned)start + count >= PHOTON_ACTIVE_SENSORS) {
        log_printf("END_COMPARE board=%u", node);
    }
}

static void compare_local(void) {
    static uint8_t buf[PHOTON_FRAME_MAX_PAYLOAD];
    for (uint8_t start = 0; start < PHOTON_ACTIVE_SENSORS; start += CAL_INFO_PER_FRAME) {
        uint8_t len = cal_session_build_info(start, CAL_INFO_PER_FRAME, buf);
        print_info_payload(g_config.node_id, buf, len);
    }
}

void calview_compare(const uint8_t *ids, int n, uint32_t delay_ms) {
    V.cmp.n = 0;
    V.cmp.next = 0;
    V.cmp.at = make_timeout_time_ms(delay_ms);
    if (n == 0) {
        if (V.sensor_role) {
            compare_local();
        }
        if (V.is_bridge) {
            const photon_node_slot_t *t = protocol_node_table();
            for (int id = 1; id <= PHOTON_MAX_NODE_ID; id++) {
                if (t[id].alive && !is_local((uint8_t)id)) {
                    V.cmp.ids[V.cmp.n++] = (uint8_t)id;
                }
            }
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        if (is_local(ids[i])) {
            compare_local();
        } else if (!V.is_bridge) {
            log_note("cal compare: this board is node %u; other boards need the bridge",
                     g_config.node_id);
        } else if (ids[i] >= 1 && ids[i] <= PHOTON_MAX_NODE_ID &&
                   V.cmp.n < PHOTON_MAX_NODE_ID) {
            V.cmp.ids[V.cmp.n++] = ids[i];
        }
    }
}

static void compare_step(void) {
    if (V.cmp.next >= V.cmp.n || !time_reached(V.cmp.at)) {
        return;
    }
    uint8_t id = V.cmp.ids[V.cmp.next++];
    for (uint8_t start = 0; start < PHOTON_ACTIVE_SENSORS; start += CAL_INFO_PER_FRAME) {
        uint8_t p[2] = { start, CAL_INFO_PER_FRAME };
        if (!protocol_bridge_request(PHOTON_FT_CAL_INFO_REQ, id, p, 2)) {
            log_note("cal compare: request queue full for board %u, retry", id);
            break;
        }
    }
    V.cmp.at = make_timeout_time_ms(COMPARE_GAP_MS);
}

// ---------------------------------------------------------------------------
// Swing dump
// ---------------------------------------------------------------------------

void calview_dump(uint8_t node, int key) {
    if (V.dump.active) {
        log_note("cal dump: one is already running");
        return;
    }
    bool local = is_local(node);
    if (!local && !V.is_bridge) {
        log_note("cal dump: this board is node %u; other boards need the bridge",
                 g_config.node_id);
        return;
    }
    if (key >= PHOTON_ACTIVE_SENSORS) {
        log_note("cal dump: key 0-%d", PHOTON_ACTIVE_SENSORS - 1);
        return;
    }
    V.dump.active = true;
    V.dump.local = local;
    V.dump.node = node;
    V.dump.key = key < 0 ? 0 : (uint8_t)key;
    V.dump.key_last = key < 0 ? PHOTON_ACTIVE_SENSORS - 1 : (uint8_t)key;
    V.dump.offset = 0;
    V.dump.tries = 0;
    V.dump.deadline = get_absolute_time();
}

static void dump_next_key(void) {
    V.dump.offset = 0;
    V.dump.tries = 0;
    V.dump.deadline = get_absolute_time();
    if (V.dump.key >= V.dump.key_last) {
        log_printf("END_DUMP board=%u", V.dump.node);
        V.dump.active = false;
        return;
    }
    V.dump.key++;
}

static void dump_payload(uint8_t node, const uint8_t *p, uint8_t len) {
    static char buf[1024];
    cal_swing_hdr_t h;
    if (!V.dump.active || node != V.dump.node || len < sizeof h) {
        return;
    }
    memcpy(&h, p, sizeof h);
    if (h.key != V.dump.key || h.offset != V.dump.offset) {
        return;  // a late reply to a retried request
    }
    uint8_t n = h.n;
    if (n > (len - sizeof h) / 2) {
        n = (uint8_t)((len - sizeof h) / 2);
    }
    if (h.offset == 0) {
        if (!(h.flags & CAL_SWING_VALID) || h.len == 0) {
            log_printf("# board %u key %u: no swing captured (%s)", node, h.key,
                       status_word(h.status));
            dump_next_key();
            return;
        }
        char nm[8];
        int16_t note = midi_map_note(node, h.key);
        bool knee = (h.flags & CAL_SWING_KNEE) != 0;
        int span = (int)h.top - (int)h.rest;
        int pct = knee && span != 0 ? ((int)h.knee_value - (int)h.rest) * 100 / span : 0;
        log_printf("BEGIN_SWING board=%u key=%u note=%s midi=%d status=%s knee=%u knee_idx=%u "
                   "knee_value=%u knee_pct=%d rest=%u top=%u pre=%u n=%u period_us=%u "
                   "truncated=%u adjacent=%u",
                   node, h.key, note_str(note, nm, sizeof nm), note, status_word(h.status),
                   knee ? 1 : 0, h.knee_idx, h.knee_value, pct, h.rest, h.top, h.pre, h.len,
                   h.period_us, (h.flags & CAL_SWING_TRUNCATED) ? 1 : 0,
                   (h.flags & CAL_SWING_ADJACENT) ? 1 : 0);
    }
    int off = 0;
    for (uint8_t i = 0; i < n && off < (int)sizeof buf - 16; i++) {
        uint16_t v;
        memcpy(&v, &p[sizeof h + i * 2u], 2);
        off += snprintf(buf + off, sizeof buf - (size_t)off, "%u,%u\r\n",
                        (unsigned)(h.offset + i), v);
    }
    log_cdc_write_all((const uint8_t *)buf, (size_t)off);
    V.dump.offset = (uint16_t)(V.dump.offset + n);
    V.dump.tries = 0;
    V.dump.deadline = get_absolute_time();
    if (n == 0 || V.dump.offset >= h.len) {
        log_printf("END_SWING");
        dump_next_key();
    }
}

static void dump_step(void) {
    if (!V.dump.active || !time_reached(V.dump.deadline)) {
        return;
    }
    if (V.dump.local) {
        static uint8_t buf[PHOTON_FRAME_MAX_PAYLOAD];
        // One chunk per loop: keeps each write inside what the CDC FIFO
        // drains before log_cdc_write_all gives up on a slow host.
        uint8_t len = cal_session_build_swing(V.dump.key, V.dump.offset, buf);
        dump_payload(V.dump.node, buf, len);
        return;
    }
    if (V.dump.tries >= SWING_TRIES) {
        log_printf("# board %u key %u: no reply", V.dump.node, V.dump.key);
        dump_next_key();
        if (!V.dump.active) {
            return;
        }
    }
    uint8_t p[3] = { V.dump.key, (uint8_t)(V.dump.offset & 0xFF), (uint8_t)(V.dump.offset >> 8) };
    if (protocol_bridge_request(PHOTON_FT_CAL_SWING_REQ, V.dump.node, p, 3)) {
        V.dump.tries++;
        V.dump.deadline = make_timeout_time_ms(SWING_REPLY_MS);
    }
}

// ---------------------------------------------------------------------------

bool calview_on_response(const photon_frame_t *f) {
    switch (f->type) {
    case PHOTON_FT_CAL_STATUS_RESP: {
        if (f->src < 1 || f->src > PHOTON_MAX_NODE_ID || f->len < 2) {
            return true;
        }
        node_state_t *ns = &V.node[f->src];
        uint8_t n = f->payload[1];
        if (n > PHOTON_MAX_SENSORS) {
            n = PHOTON_MAX_SENSORS;
        }
        if (n > (f->len - 2) / 2) {
            n = (uint8_t)((f->len - 2) / 2);
        }
        ns->learning = (f->payload[0] & CAL_FLAG_LEARNING) != 0;
        ns->global = (f->payload[0] & CAL_FLAG_GLOBAL) != 0;
        ns->seen = get_absolute_time();
        memset(ns->status, CAL_KEY_DISABLED, sizeof ns->status);
        memcpy(ns->status, &f->payload[2], n);
        return true;
    }
    case PHOTON_FT_CAL_INFO_RESP:
        print_info_payload(f->src, f->payload, f->len);
        return true;
    case PHOTON_FT_CAL_SWING_RESP:
        dump_payload(f->src, f->payload, f->len);
        return true;
    default:
        return false;
    }
}

void calview_task(const char *line, size_t line_len) {
    dump_step();
    compare_step();
    if (!V.showing) {
        return;
    }
    if (V.is_bridge && time_reached(V.next_poll)) {
        V.next_poll = make_timeout_time_ms(POLL_MS);
        const photon_node_slot_t *t = protocol_node_table();
        for (int id = 1; id <= PHOTON_MAX_NODE_ID; id++) {
            if (t[id].alive && !is_local((uint8_t)id)) {
                protocol_bridge_request(PHOTON_FT_CAL_STATUS_REQ, (uint8_t)id, NULL, 0);
            }
        }
    }
    if (time_reached(V.next_draw)) {
        V.next_draw = make_timeout_time_ms(DRAW_MS);
        if (log_console_connected() && !V.dump.active) {
            draw(line, line_len);
        }
    }
}
