#ifndef PHOTON_MIDI_OUT_H
#define PHOTON_MIDI_OUT_H

#include <stdint.h>

void midi_out_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
void midi_out_note_off(uint8_t channel, uint8_t note, uint8_t velocity);

#endif
