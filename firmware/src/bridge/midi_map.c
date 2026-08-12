#include "bridge/midi_map.h"

#include <math.h>
#include <string.h>

#include "board_config.h"
#include "config/config_store.h"
#include "usb/cdc_console.h"
#include "usb/midi_out.h"

#define MANUAL_COUNT ((PHOTON_GLOBAL_SENSORS + PHOTON_SENSORS_PER_MANUAL - 1) / \
                      PHOTON_SENSORS_PER_MANUAL)

static int16_t note_for_global[PHOTON_GLOBAL_SENSORS];
static uint8_t channel_for_global[PHOTON_GLOBAL_SENSORS];
// OR-group refcount per (channel, note): several sensors may map to one key.
static uint8_t group_refcount[MANUAL_COUNT][128];
// Which sensors each node currently holds ON (dedup + dropout release).
static uint32_t node_notes_on[PHOTON_MAX_NODE_ID + 1];
static uint32_t notes_on_sent, notes_off_sent;

static inline bool global_disabled(uint32_t g) {
    return (g_config.global_disabled[g / 8] >> (g % 8)) & 1u;
}

void midi_map_init(void) {
    // Per-manual linear assignment low..high over enabled sensors — the
    // legacy build_index_to_midi_by_manual(): the note range and channel
    // restart every PHOTON_SENSORS_PER_MANUAL global sensors.
    memset(group_refcount, 0, sizeof group_refcount);
    memset(node_notes_on, 0, sizeof node_notes_on);
    for (uint32_t g = 0; g < PHOTON_GLOBAL_SENSORS; g++) {
        uint32_t manual = g / PHOTON_SENSORS_PER_MANUAL;
        note_for_global[g] = -1;
        channel_for_global[g] = (uint8_t)((g_config.midi_channel + manual) & 0x0F);
    }
    for (uint32_t manual = 0; manual < MANUAL_COUNT; manual++) {
        int note = g_config.midi_low;
        uint32_t base = manual * PHOTON_SENSORS_PER_MANUAL;
        for (uint32_t k = 0; k < PHOTON_SENSORS_PER_MANUAL &&
                             base + k < PHOTON_GLOBAL_SENSORS; k++) {
            uint32_t g = base + k;
            if (global_disabled(g) || note > g_config.midi_high) {
                continue;
            }
            note_for_global[g] = (int16_t)note;
            note++;
        }
    }
}

int16_t midi_map_note(uint8_t node_id, uint8_t local_idx) {
    if (node_id < 1 || node_id > PHOTON_MAX_NODE_ID ||
        local_idx >= PHOTON_MAX_SENSORS) {
        return -1;
    }
    return note_for_global[(uint32_t)(node_id - 1) * PHOTON_MAX_SENSORS + local_idx];
}

uint8_t midi_map_velocity(uint32_t dt_us) {
    float dt_ms = (float)dt_us / 1000.0f;
    if (dt_ms <= g_config.vel_min_ms) {
        return 127;
    }
    if (dt_ms >= g_config.vel_max_ms) {
        return 1;
    }
    float span = g_config.vel_max_ms - g_config.vel_min_ms;
    float x = (g_config.vel_max_ms - dt_ms) / span;
    float v = 1.0f + 126.0f * powf(x, g_config.vel_curve);
    if (v < 1.0f) {
        v = 1.0f;
    }
    if (v > 127.0f) {
        v = 127.0f;
    }
    return (uint8_t)v;  // truncation, matching the legacy int() conversion
}

static void group_off(uint32_t manual, uint8_t channel, uint8_t note, uint8_t velocity) {
    if (group_refcount[manual][note] > 0 && --group_refcount[manual][note] == 0) {
        midi_out_note_off(channel, note, velocity);
        notes_off_sent++;
    }
}

void midi_map_handle_event(uint8_t node_id, const photon_event_t *ev) {
    int16_t note = midi_map_note(node_id, ev->local_idx);
    if (note < 0 || node_id < 1 || node_id > PHOTON_MAX_NODE_ID) {
        return;
    }
    uint32_t g = (uint32_t)(node_id - 1) * PHOTON_MAX_SENSORS + ev->local_idx;
    uint32_t manual = g / PHOTON_SENSORS_PER_MANUAL;
    uint8_t channel = channel_for_global[g];
    uint32_t bit = 1u << ev->local_idx;

    console_print_event(node_id, ev, note, midi_map_velocity(ev->dt_us), channel);

    if (ev->state) {
        if (node_notes_on[node_id] & bit) {
            return;  // duplicate ON (node rebooted mid-note, etc.)
        }
        node_notes_on[node_id] |= bit;
        if (group_refcount[manual][note]++ == 0) {
            midi_out_note_on(channel, (uint8_t)note, midi_map_velocity(ev->dt_us));
            notes_on_sent++;
        }
    } else {
        if (!(node_notes_on[node_id] & bit)) {
            return;  // unmatched OFF
        }
        node_notes_on[node_id] &= ~bit;
        // Release velocity from the release dt, as in the legacy host.
        group_off(manual, channel, (uint8_t)note, midi_map_velocity(ev->dt_us));
    }
}

void midi_map_release_node(uint8_t node_id) {
    if (node_id < 1 || node_id > PHOTON_MAX_NODE_ID) {
        return;
    }
    uint32_t held = node_notes_on[node_id];
    node_notes_on[node_id] = 0;
    for (uint8_t idx = 0; held != 0; idx++, held >>= 1) {
        if (!(held & 1u)) {
            continue;
        }
        int16_t note = midi_map_note(node_id, idx);
        if (note < 0) {
            continue;
        }
        uint32_t g = (uint32_t)(node_id - 1) * PHOTON_MAX_SENSORS + idx;
        group_off(g / PHOTON_SENSORS_PER_MANUAL, channel_for_global[g],
                  (uint8_t)note, 64);
    }
}

uint32_t midi_map_notes_on_sent(void) { return notes_on_sent; }
uint32_t midi_map_notes_off_sent(void) { return notes_off_sent; }
