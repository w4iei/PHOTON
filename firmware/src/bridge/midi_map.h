// Bridge-role event -> MIDI mapping: global sensor index -> note (linear
// range with disabled sensors skipped), dt -> velocity curve, and OR-group
// note arbitration — ported from rs485_main_host/midi_mapping.py and
// event_mode.py, now fed microsecond dt.
#ifndef PHOTON_MIDI_MAP_H
#define PHOTON_MIDI_MAP_H

#include <stdint.h>

#include "ipc/rings.h"

// Build the note table from g_config (call after config_store_init and
// again after any config change).
void midi_map_init(void);

// -1 if the sensor is unmapped/disabled.
int16_t midi_map_note(uint8_t node_id, uint8_t local_idx);

// dt -> MIDI velocity 1..127 via the configured curve.
uint8_t midi_map_velocity(uint32_t dt_us);

// Full event path: dedup/arbitrate and emit USB-MIDI. Wired as the
// protocol event sink on the bridge.
void midi_map_handle_event(uint8_t node_id, const photon_event_t *ev);

// Counters for the console.
uint32_t midi_map_notes_on_sent(void);
uint32_t midi_map_notes_off_sent(void);

#endif
