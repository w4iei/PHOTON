#include "bridge/midi_map.h"

#include <math.h>
#include <string.h>

#include "board_config.h"
#include "config/config_store.h"
#include "usb/midi_out.h"

static int16_t note_for_global[PHOTON_GLOBAL_SENSORS];
static uint8_t group_refcount[128];  // sensors currently ON per note
static uint32_t notes_on_sent, notes_off_sent;

static inline bool global_disabled(uint32_t g) {
    return (g_config.global_disabled[g / 8] >> (g % 8)) & 1u;
}

void midi_map_init(void) {
    // Linear assignment low..high over enabled sensors, identical to
    // build_index_to_midi_linear(): global order, disabled skipped.
    memset(group_refcount, 0, sizeof group_refcount);
    int note = g_config.midi_low;
    for (uint32_t g = 0; g < PHOTON_GLOBAL_SENSORS; g++) {
        if (global_disabled(g) || note > g_config.midi_high) {
            note_for_global[g] = -1;
            continue;
        }
        note_for_global[g] = (int16_t)note;
        note++;
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
    return (uint8_t)(v + 0.5f);
}

void midi_map_handle_event(uint8_t node_id, const photon_event_t *ev) {
    int16_t note = midi_map_note(node_id, ev->local_idx);
    if (note < 0) {
        return;
    }
    // OR-group arbitration: several sensors may map to one note; NoteOn on
    // the group's 0->1 transition, NoteOff on 1->0.
    if (ev->state) {
        if (group_refcount[note]++ == 0) {
            midi_out_note_on(g_config.midi_channel, (uint8_t)note,
                             midi_map_velocity(ev->dt_us));
            notes_on_sent++;
        }
    } else {
        if (group_refcount[note] > 0 && --group_refcount[note] == 0) {
            midi_out_note_off(g_config.midi_channel, (uint8_t)note, 64);
            notes_off_sent++;
        }
    }
}

uint32_t midi_map_notes_on_sent(void) { return notes_on_sent; }
uint32_t midi_map_notes_off_sent(void) { return notes_off_sent; }
