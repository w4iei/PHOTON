#include "usb/midi_out.h"

#include "pico/time.h"
#include "tusb.h"

#include "bridge/recorder.h"

static void send3(uint8_t b0, uint8_t b1, uint8_t b2) {
    // The card recorder sees every message, host or no host.
    recorder_push((uint32_t)(time_us_64() / 1000u), b0, b1, b2);
    if (!tud_midi_mounted()) {
        return;
    }
    uint8_t msg[3] = { b0, b1, b2 };
    tud_midi_stream_write(0, msg, 3);
}

void midi_out_note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    send3((uint8_t)(0x90 | (channel & 0x0F)), note & 0x7F, velocity & 0x7F);
}

void midi_out_note_off(uint8_t channel, uint8_t note, uint8_t velocity) {
    send3((uint8_t)(0x80 | (channel & 0x0F)), note & 0x7F, velocity & 0x7F);
}
