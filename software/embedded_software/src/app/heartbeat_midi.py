"""Simple test that toggles C4 on/off (run mode 'heartbeat')."""

import time
import usb_midi
import adafruit_midi
from adafruit_midi.note_on import NoteOn
from adafruit_midi.note_off import NoteOff


def main():
    midi = adafruit_midi.MIDI(midi_out=usb_midi.ports[1], out_channel=0)

    while True:
        midi.send(NoteOn(60, 120))  # C4 on
        time.sleep(0.25)
        midi.send(NoteOff(60, 0))   # C4 off
        time.sleep(0.25)

if __name__ == "__main__":
    main()
