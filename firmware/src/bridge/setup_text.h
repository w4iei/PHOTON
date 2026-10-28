// SETUP.TXT: the settings a power-on's recordings were played with, as
// plain text next to them on the microSD card. bridge/setup_log.c gathers
// the values on core 0; the text is built here, apart from the bus, so the
// host tests can check it.
#ifndef PHOTON_SETUP_TEXT_H
#define PHOTON_SETUP_TEXT_H

#include <stddef.h>
#include <stdint.h>

#include "board_config.h"
#include "cal/cal_session.h"

#define SETUP_TEXT_MAX 12288  // six boards of 31 keys fit with room to spare

typedef enum {
    SETUP_NODE_ABSENT = 0,  // not on the bus when the boards were read
    SETUP_NODE_READ,        // every page of its table arrived
    SETUP_NODE_NO_REPLY,    // on the bus, but its table did not arrive
} setup_node_state_t;

typedef struct {
    uint8_t state;   // setup_node_state_t
    uint8_t flags;   // CAL_FLAG_* of the board
    int16_t note[PHOTON_ACTIVE_SENSORS];      // MIDI note per key, -1 = none
    cal_info_rec_t key[PHOTON_ACTIVE_SENSORS];
} setup_node_t;

typedef struct {
    const char *build_id;
    const char *build_date;
    const char *serial;
    uint32_t read_ms;  // since power-on
    uint8_t channel[PHOTON_MAX_MANUALS];  // on the wire, 1-16
    uint8_t channel_auto[PHOTON_MAX_MANUALS];
    uint8_t midi_low;
    uint8_t midi_high;
    float vel_out_min;
    float vel_out_max;
    float vel_min_ms;
    float vel_max_ms;
    float vel_curve;
    uint8_t strike_global;  // the bridge's own copy
    knee_rules_t rules;     // the bridge's own copy
    uint8_t margin_pct;
    setup_node_t node[PHOTON_MAX_NODE_ID + 1];  // [1..PHOTON_MAX_NODE_ID]
} setup_info_t;

// Writes the text into out (NUL-terminated) and returns its length. Text
// that does not fit is cut at a line end and marked as cut.
size_t setup_text_format(char *out, size_t cap, const setup_info_t *s);

#endif
