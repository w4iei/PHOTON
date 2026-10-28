// Note-map tests: the per-manual note walk with boards mounted rotated 180
// degrees (PHOTON_REVERSED_NODES = nodes 2 and 3, set in CMakeLists.txt) and
// high-index slots cut off / disabled, as on a shortened board.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge/midi_map.h"
#include "config/config_store.h"

// midi_map.c's collaborators: the config, the event log line and USB-MIDI.
photon_config_t g_config;
void console_print_event(uint8_t node_id, const photon_event_t *ev,
                         int16_t note, uint8_t velocity, uint8_t channel) {
    (void)node_id;
    (void)ev;
    (void)note;
    (void)velocity;
    (void)channel;
}
void midi_out_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    (void)channel;
    (void)note;
    (void)velocity;
}
void midi_out_note_off(uint8_t channel, uint8_t note, uint8_t velocity) {
    (void)channel;
    (void)note;
    (void)velocity;
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

// Disable slots first..last (inclusive) of one node, by physical slot.
static void disable_slots(uint8_t node_id, int first, int last) {
    for (int s = first; s <= last; s++) {
        uint32_t g = (uint32_t)(node_id - 1) * PHOTON_MAX_SENSORS + (uint32_t)s;
        g_config.global_disabled[g / 8] |= (uint8_t)(1u << (g % 8));
    }
}

static void setup(void) {
    memset(&g_config, 0, sizeof g_config);
    g_config.midi_low = PHOTON_MIDI_LOW;    // 29, F1
    g_config.midi_high = PHOTON_MIDI_HIGH;  // 89
    g_config.midi_channel = PHOTON_MIDI_CHANNEL;
}

// Manual 0: node 1 upright (slot 31 unpopulated), node 2 rotated with slots
// 25-31 cut off. Node 1 takes 29..59 bass to treble; node 2 continues at 60
// from its highest remaining slot (24, now at its bass end) down to slot 0.
static void test_rotated_treble_board(void) {
    setup();
    disable_slots(1, 31, 31);
    disable_slots(2, 25, 31);
    midi_map_init();
    for (int s = 0; s <= 30; s++) {
        CHECK(midi_map_note(1, (uint8_t)s) == 29 + s);
    }
    CHECK(midi_map_note(1, 31) == -1);
    for (int s = 0; s <= 24; s++) {
        CHECK(midi_map_note(2, (uint8_t)s) == 60 + (24 - s));
    }
    for (int s = 25; s <= 31; s++) {
        CHECK(midi_map_note(2, (uint8_t)s) == -1);
    }
}

// Manual 1: node 3 rotated with slots 28-31 cut off, node 4 upright with
// slots 30-31 disabled. The manual's lowest note lands on node 3's highest
// remaining slot (27); node 4 carries on upward from 57.
static void test_rotated_bass_board(void) {
    setup();
    disable_slots(3, 28, 31);
    disable_slots(4, 30, 31);
    midi_map_init();
    for (int s = 0; s <= 27; s++) {
        CHECK(midi_map_note(3, (uint8_t)s) == 29 + (27 - s));
    }
    for (int s = 28; s <= 31; s++) {
        CHECK(midi_map_note(3, (uint8_t)s) == -1);
    }
    for (int s = 0; s <= 29; s++) {
        CHECK(midi_map_note(4, (uint8_t)s) == 57 + s);
    }
    CHECK(midi_map_note(4, 30) == -1 && midi_map_note(4, 31) == -1);
}

// A slot disabled in the middle of a rotated board is skipped without a gap
// in the notes, exactly as on an upright one.
static void test_rotated_board_inner_gap(void) {
    setup();
    disable_slots(2, 31, 31);
    disable_slots(2, 10, 10);
    midi_map_init();
    // Node 1 fully enabled: 29..60. Node 2 walks 30, 29, ..., 11, 9, ..., 0.
    CHECK(midi_map_note(1, 31) == 60);
    CHECK(midi_map_note(2, 30) == 61);
    CHECK(midi_map_note(2, 11) == 61 + 19);
    CHECK(midi_map_note(2, 10) == -1);
    CHECK(midi_map_note(2, 9) == 61 + 20);
    CHECK(midi_map_note(2, 1) == 89);   // midi_high still caps the range:
    CHECK(midi_map_note(2, 0) == -1);   // slot 0 would be 90
}

int main(void) {
    test_rotated_treble_board();
    test_rotated_bass_board();
    test_rotated_board_inner_gap();
    printf("test_midi_map OK (%d checks)\n", checks);
    return 0;
}
