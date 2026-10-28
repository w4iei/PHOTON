// SETUP.TXT text: bridge settings, one table per board read, boards that
// did not answer or were not on the bus, and a buffer too small for it all.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge/setup_text.h"

static int checks = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

static setup_info_t info;
static char out[SETUP_TEXT_MAX];

// Whole line present, not just a substring of one.
static bool has_line(const char *text, const char *want) {
    size_t n = strlen(want);
    for (const char *p = text; (p = strstr(p, want)) != NULL; p++) {
        if ((p == text || p[-1] == '\n') && p[n] == '\n') {
            return true;
        }
    }
    return false;
}

static void fill(void) {
    memset(&info, 0, sizeof info);
    info.build_id = "d872cb0";
    info.build_date = "2026-09-24 11:28";
    info.serial = "0123456789ABCDEF";
    info.read_ms = 3 * 3600000u + 25 * 60000u + 7 * 1000u + 999u;
    info.channel[0] = 3;
    info.channel[1] = 2;
    info.channel[2] = 4;
    info.channel_auto[2] = 1;
    info.midi_low = 29;
    info.midi_high = 89;
    info.vel_out_min = 50;
    info.vel_out_max = 120;
    info.vel_min_ms = 2.5f;
    info.vel_max_ms = 25.0f;
    info.vel_curve = 2.0f;
    info.rules = (knee_rules_t){ 40, 80, 25, 8 };
    info.margin_pct = 3;

    // Board 1: knee thresholds, one disabled key, one uncalibrated key.
    setup_node_t *b1 = &info.node[1];
    b1->state = SETUP_NODE_READ;
    for (int i = 0; i < PHOTON_ACTIVE_SENSORS; i++) {
        b1->note[i] = (int16_t)(29 + i);
        b1->key[i] = (cal_info_rec_t){ .status = CAL_KEY_UNSEEN, .new_strike = 70,
                                       .new_min = 3000, .new_max = 43000 };
    }
    b1->key[0].new_strike = 79;
    b1->key[0].new_min = 5118;
    b1->key[0].new_max = 48258;
    b1->key[5].new_strike = 0;  // no knee: the global threshold
    b1->key[29].status = CAL_KEY_DISABLED;
    b1->note[30] = -1;
    b1->key[30].new_strike = 0;
    b1->key[30].new_min = 0xFFFF;
    b1->key[30].new_max = 0;

    // Board 2: strike global, read while calibrating.
    setup_node_t *b2 = &info.node[2];
    b2->state = SETUP_NODE_READ;
    b2->flags = CAL_FLAG_GLOBAL | CAL_FLAG_LEARNING;
    for (int i = 0; i < PHOTON_ACTIVE_SENSORS; i++) {
        b2->note[i] = (int16_t)(60 + i);
        b2->key[i] = (cal_info_rec_t){ .new_min = 3000, .new_max = 43000 };
    }

    info.node[3].state = SETUP_NODE_NO_REPLY;
    // 4, 5, 6 absent
}

static void test_text(void) {
    fill();
    size_t n = setup_text_format(out, sizeof out, &info);
    CHECK(n == strlen(out) && n > 0 && n < sizeof out);
    CHECK(out[n - 1] == '\n');
    const char *first = "PHOTON setup for the recordings in this directory\n";
    CHECK(strncmp(out, first, strlen(first)) == 0);
    CHECK(strstr(out, "Read at power-on +03:25:07;") != NULL);
    CHECK(has_line(out, "bridge      fw d872cb0 (2026-09-24 11:28), hw 0123456789ABCDEF"));
    CHECK(has_line(out, "channels    manual 1 (boards 1-2) on channel 3"));
    CHECK(has_line(out, "            manual 2 (boards 3-4) on channel 2"));
    CHECK(has_line(out, "            manual 3 (boards 5-6) on channel 4 (auto)"));
    CHECK(has_line(out, "notes       29-89 on each manual"));
    CHECK(has_line(out, "velocity    velrange 50-120, velcurve 2.5 25.0 2.00"));
    CHECK(strstr(out, "\nstrike      knee (the bridge's copy") != NULL);
    CHECK(strstr(out, "\nknee rules  40-80% of the travel, 2.5x, 8%, +3%") != NULL);

    CHECK(has_line(out, "== board 1: strike knee, per-key thresholds =="));
    CHECK(has_line(out, "idx note   min   max thr%    thr"));
    CHECK(has_line(out, "  0 F1    5118 48258  79   39198"));  // 5118 + 43140 * 79 / 100
    CHECK(has_line(out, "  5 A#1   3000 43000  60g  27000"));
    CHECK(has_line(out, "  6 B1    3000 43000  70   31000"));
    CHECK(has_line(out, " 29 A#3  disabled"));
    CHECK(has_line(out, " 30 -        -     -  60g      -"));

    CHECK(has_line(out, "== board 2: strike global, every key at the fixed threshold"
                        " (calibrating: values as a save would store them) =="));
    CHECK(has_line(out, "  0 C4    3000 43000  60g  27000"));
    CHECK(has_line(out, "== board 3: on the bus, but its calibration did not arrive =="));
    CHECK(strstr(out, "== board 4") == NULL);
    CHECK(has_line(out, "boards not on the bus: 4 5 6"));
    CHECK(strstr(out, "cut here") == NULL);

    // Header and a full table for each board read: every key once.
    int rows = 0;
    for (const char *p = out; (p = strchr(p, '\n')) != NULL; p++) {
        rows++;
    }
    CHECK(rows > 2 * PHOTON_ACTIVE_SENSORS + 10);
}

static void test_bridge_alone(void) {
    fill();
    for (int id = 1; id <= PHOTON_MAX_NODE_ID; id++) {
        info.node[id].state = SETUP_NODE_ABSENT;
    }
    info.strike_global = 1;
    size_t n = setup_text_format(out, sizeof out, &info);
    CHECK(n == strlen(out));
    CHECK(strstr(out, "== board") == NULL);
    CHECK(has_line(out, "boards not on the bus: 1 2 3 4 5 6"));
    CHECK(strstr(out, "\nstrike      global (the bridge's copy") != NULL);
}

static void test_cut(void) {
    fill();
    char small[600];
    memset(small, 'x', sizeof small);
    size_t n = setup_text_format(small, sizeof small, &info);
    CHECK(n < sizeof small && n == strlen(small));
    CHECK(strstr(small, "... (cut here: the text is longer than the buffer)\n") != NULL);
    CHECK(small[n - 1] == '\n');
    // Whole lines only: the cut marker is the last line.
    const char *cut = strstr(small, "... (cut here");
    CHECK(cut > small && cut[-1] == '\n');

    char tiny[8];
    CHECK(setup_text_format(tiny, sizeof tiny, &info) == 0 && tiny[0] == '\0');
    CHECK(setup_text_format(tiny, 0, &info) == 0);
}

int main(void) {
    test_text();
    test_bridge_alone();
    test_cut();
    printf("test_setup_text: %d checks passed\n", checks);
    return 0;
}
