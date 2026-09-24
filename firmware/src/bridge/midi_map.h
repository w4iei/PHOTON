// Bridge-role event -> MIDI mapping, ported from rs485_main_host
// midi_mapping.py + event_mode.py, fed microsecond dt:
//   - note range (midi_low..midi_high) restarts for every manual
//     (PHOTON_SENSORS_PER_MANUAL global sensors), skipping disabled sensors
//   - each manual gets its own MIDI channel (midi_channel + manual index)
//   - dt -> velocity curve applies to NoteOn AND NoteOff (release speed)
//   - OR-group arbitration per (manual, note), tracked per node so a node
//     dropout releases its held notes instead of sticking them
#ifndef PHOTON_MIDI_MAP_H
#define PHOTON_MIDI_MAP_H

#include <stdbool.h>
#include <stdint.h>

#include "ipc/rings.h"

// Build the note/channel tables from g_config (call after config_store_init
// and again after any config change).
void midi_map_init(void);
uint8_t midi_map_channel_for_manual(uint32_t manual);

// -1 if the sensor is unmapped/disabled.
int16_t midi_map_note(uint8_t node_id, uint8_t local_idx);

// dt -> MIDI velocity via the configured curve (legacy truncation), with
// the output compressed into vel_out_min..vel_out_max. Used for release
// velocity too, from the release dt.
uint8_t midi_map_velocity(uint32_t dt_us);

// Full event path: dedup/arbitrate and emit USB-MIDI. Wired as the
// protocol event sink on the bridge.
void midi_map_handle_event(uint8_t node_id, const photon_event_t *ev);

// A node left the bus (silent/rebooted): release every note it holds.
// Wired as the protocol node-down callback on the bridge.
void midi_map_release_node(uint8_t node_id);

// Coupler evidence, per manual and note: set when a note went down while
// the same note was already held on another manual (a coupled double moves
// the other manual's key with it), cleared by that note's next press alone.
// The calibration view marks such keys; a coupler-status register would
// build on the same signal.
bool midi_map_coupled(uint32_t manual, uint8_t note);
void midi_map_clear_coupled(void);

// Counters for the console.
uint32_t midi_map_notes_on_sent(void);
uint32_t midi_map_notes_off_sent(void);

#endif
