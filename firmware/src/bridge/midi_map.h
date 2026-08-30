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

// Counters for the console.
uint32_t midi_map_notes_on_sent(void);
uint32_t midi_map_notes_off_sent(void);

#endif
