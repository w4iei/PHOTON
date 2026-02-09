"""USB MIDI file playback helper (flash or microSD)."""

import os
import usb_midi
import adafruit_midi
try:
    from adafruit_midi.note_on import NoteOn
    from adafruit_midi.note_off import NoteOff
    from adafruit_midi.control_change import ControlChange
    from adafruit_midi.program_change import ProgramChange
except ImportError:
    try:
        from adafruit_midi import NoteOn, NoteOff, ControlChange, ProgramChange
    except ImportError:
        # Fallback for stripped bundles: minimal message classes that emit raw bytes.
        class _BaseMsg:
            def __init__(self, status, data1, data2=None, channel=None):
                ch = 0 if channel is None else channel
                if data2 is None:
                    self.message = bytes([status | ch, data1 & 0x7F])
                else:
                    self.message = bytes([status | ch, data1 & 0x7F, data2 & 0x7F])

        class NoteOn(_BaseMsg):
            def __init__(self, note, velocity=127, *, channel=None):
                super().__init__(0x90, note, velocity, channel)

        class NoteOff(_BaseMsg):
            def __init__(self, note, velocity=0, *, channel=None):
                super().__init__(0x80, note, velocity, channel)

        class ControlChange(_BaseMsg):
            def __init__(self, control, value, *, channel=None):
                super().__init__(0xB0, control, value, channel)

        class ProgramChange(_BaseMsg):
            def __init__(self, program, *, channel=None):
                super().__init__(0xC0, program, None, channel)

try:
    from adafruit_midi import MIDI  # type: ignore
except Exception:
    # Minimal fallback that writes raw bytes to the selected USB MIDI port.
    class MIDI:
        def __init__(self, *, midi_out, out_channel=0):
            self.midi_out = midi_out
            self.out_channel = out_channel

        def send(self, message):
            try:
                payload = message.message  # our minimal Note* classes expose .message
            except Exception:
                payload = bytes(message)
            self.midi_out.write(payload)
import adafruit_midi_parser
import time, gc
import board
import digitalio
import time
from pwmio import PWMOut


MIDI_OUT_PORT = 1   # usb_midi.ports[1] is the usual user-facing MIDI OUT
MOUNT_POINT = "/sd"
MIDI_EXTENSIONS = (".mid", ".midi")

class USBOutPlayer(adafruit_midi_parser.MIDIPlayer):
    """
    USBOutPlayer is a MIDI player that sends MIDI messages over USB.
    ignore_program_change=True means it will not send Program Change messages, set to False if you have an actual
    recording from a device that is set up for Piano Teq
    """
    def __init__(self, parser, out_channel=0, ignore_program_change=True):
        super().__init__(parser)
        self.midi = adafruit_midi.MIDI(midi_out=usb_midi.ports[MIDI_OUT_PORT],
                                       out_channel=out_channel)
        # Optional: auto-loop between songs
        self.loop_playback = True
        self.restart_delay = 1.0
        self.ignore_program_change = ignore_program_change

    # Map parser callbacks to real USB MIDI messages
    def on_note_on(self, note, velocity, channel):
        self.midi.send(NoteOn(note, velocity, channel=channel))

    def on_note_off(self, note, velocity, channel):
        self.midi.send(NoteOff(note, velocity, channel=channel))

    def on_controller(self, controller, value, channel):
        self.midi.send(ControlChange(controller, value, channel=channel))

    def on_program_change(self, program, channel):
        if not self.ignore_program_change:
            self.midi.send(ProgramChange(program, channel=channel))

# LED controls # Todo: move.


def _solid_on(led):
    led.value = True

def _blink_for(led, seconds=2.0, period=0.08):
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        led.value = not led.value
        time.sleep(period)
    led.value = False


def strobe_led_while(led, fn, *args, led_pin=board.LED, pwm_pin=None, **kwargs):
    """
    If pwm_pin is provided (must be an MCU GPIO), strobe via PWM while fn runs.
    Otherwise: fast-blink led_pin briefly, run fn (blocking), then set solid ON.
    Returns fn(*args, **kwargs).
    """
    if pwm_pin is not None:
        try:
            led_pwm = PWMOut(pwm_pin, frequency=12, duty_cycle=32768)  # ~12 Hz strobe
            try:
                result = fn(*args, **kwargs)
            finally:
                led_pwm.deinit()
                _solid_on(led_pin)
            return result
        except Exception:
            # If PWM setup fails, fall back to non-PWM path
            pass

    # No PWM possible on CywPin: do a quick pre-parse blink, then solid after
    _blink_for(led, seconds=2.0, period=0.06)
    result = fn(*args, **kwargs)
    _solid_on(led)
    return result


def main(filename="/assets/bwv832.mid", channel=0):
    # Green LED on.
    from app.helpers import hardware

    led = hardware.claim_output(board.LED)

    # _blink_for(led, seconds=1.0, period=0.06)

    parser = adafruit_midi_parser.MIDIParser()
    parser.parse(filename)
    # strobe_led_while(led, parser.parse, filename)
    print("Parsed", filename, "events:", len(parser.events), "BPM:", parser.bpm)
    player = USBOutPlayer(parser, out_channel=channel)
    # _solid_on(led)

    # Main loop: call play() repeatedly; it handles timing internally.
    while True:
        player.play(loop=True)


def main2(filename='', channel=0):
    # CircuitPython / MicroPython-safe streaming SMF Type-0 player
    import time, gc, usb_midi, adafruit_midi

    BPM = 250  # fixed playback tempo

    # Make TICK_US mutable so we can set it after reading the file header (PPQ).
    TICK_US = [None]

    midi = MIDI(midi_out=usb_midi.ports[1], out_channel=channel)

    def _read_u16_be(f):
        b = f.read(2)
        return (b[0] << 8) | b[1]

    def _read_u32_be(f):
        b = f.read(4)
        return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]

    def _read_varlen(f):
        """Standard MIDI variable-length quantity."""
        value = 0
        while True:
            b = f.read(1)
            if not b:  # EOF
                return None
            b = b[0]
            value = (value << 7) | (b & 0x7F)
            if not (b & 0x80):
                return value

    def _expect(f, tag: bytes):
        if f.read(4) != tag:
            raise ValueError("Missing tag %r" % tag)

    def _set_tick_us_from_division(division):
        # Refuse SMPTE time code (high bit set)
        if division & 0x8000:
            raise ValueError("SMPTE time division not supported")
        # ticks-per-quarter -> microseconds per tick
        TICK_US[0] = int(60_000_000 / (BPM * division))

    def stream_type0_note_events(path):
        """
        Yield tuples (delta_ticks, status, note, vel) from a Type-0 MIDI file.
        Ignores tempo/meta and all non-note messages.
        """
        with open(path, "rb") as f:
            _expect(f, b"MThd")
            hdr_len = _read_u32_be(f)
            fmt = _read_u16_be(f)      # must be 0
            ntr = _read_u16_be(f)      # must be 1
            division = _read_u16_be(f) # ticks per quarter (ignore SMPTE)
            if hdr_len > 6:
                f.read(hdr_len - 6)

            if fmt != 0 or ntr != 1:
                raise ValueError("Export as Type-0 (single track) first. Got fmt=%d tracks=%d" % (fmt, ntr))

            # compute tick duration from file PPQ
            _set_tick_us_from_division(division)

            _expect(f, b"MTrk")
            track_len = _read_u32_be(f)
            track_end = f.tell() + track_len

            running_status = None
            while f.tell() < track_end:
                delta = _read_varlen(f)
                if delta is None:
                    break
                b = f.read(1)
                if not b:
                    break
                status = b[0]
                if status < 0x80:
                    # running status: this byte is actually data1
                    if running_status is None:
                        break
                    data1 = status
                    status = running_status
                else:
                    running_status = status
                    data1 = f.read(1)[0]

                # Meta & SysEx handling (skip, but keep timing)
                if status == 0xFF:
                    meta_len = _read_varlen(f)
                    f.read(meta_len or 0)
                    yield (delta, None, None, None)
                    continue
                if status in (0xF0, 0xF7):
                    syx_len = _read_varlen(f)
                    f.read(syx_len or 0)
                    yield (delta, None, None, None)
                    continue

                # Channel message
                msg_type = status & 0xF0
                if msg_type in (0xC0, 0xD0):
                    # 1 data byte messages we don't use
                    yield (delta, None, None, None)
                    continue

                data2 = f.read(1)[0]

                if msg_type == 0x90:  # note on
                    note, vel = data1 & 0x7F, data2 & 0x7F
                    if vel > 0:
                        yield (delta, 0x90, note, vel)
                    else:
                        # Treat note_on with vel=0 as note_off
                        yield (delta, 0x80, note, 0)
                elif msg_type == 0x80:  # note off
                    note, vel = data1 & 0x7F, data2 & 0x7F
                    yield (delta, 0x80, note, vel)
                else:
                    # Other channel messages ignored; preserve timing
                    yield (delta, None, None, None)

    def play_stream(path):
        tick_time_us = time.monotonic_ns() // 1000
        gc_ctr = 0
        for delta, status, note, vel in stream_type0_note_events(path):
            if delta:
                tick_time_us += delta * TICK_US[0]
                # busy-wait; replace with a tiny sleep if you prefer
                while (time.monotonic_ns() // 1000) < tick_time_us:
                    pass
            if status == 0x90:
                midi.send(NoteOn(note, vel))
            elif status == 0x80:
                midi.send(NoteOff(note, vel))

            gc_ctr += 1
            if gc_ctr & 63 == 0:  # every 64 events
                gc.collect()

    # go
    while True:  # Keep playing forever.
        play_stream(filename)


def main_stream_type1(filename='', channel=0):
    """
    Streaming SMF Type-1 player (interleaves multiple tracks, low-memory).
    Supports tempo changes (meta 0x51) and NoteOn/NoteOff. Other channel messages
    are ignored for simplicity.
    """
    import time, usb_midi, adafruit_midi

    midi = MIDI(midi_out=usb_midi.ports[1], out_channel=channel)

    def _read_u16_be(f):
        b = f.read(2)
        return (b[0] << 8) | b[1]

    def _read_u32_be(f):
        b = f.read(4)
        return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]

    def _read_varlen(f):
        value = 0
        while True:
            b = f.read(1)
            if not b:
                return None
            b = b[0]
            value = (value << 7) | (b & 0x7F)
            if not (b & 0x80):
                return value

    class TrackState:
        __slots__ = ("start", "end", "pos", "abs_tick", "next_tick", "running", "next_event")

        def __init__(self, start, length):
            self.start = start
            self.end = start + length
            self.pos = start
            self.abs_tick = 0
            self.next_tick = None  # absolute tick of next event
            self.running = None
            self.next_event = (None, None, None)

    def _init_tracks(path, tick_us_ref):
        with open(path, "rb") as f:
            if f.read(4) != b"MThd":
                raise ValueError("Missing MThd")
            hdr_len = _read_u32_be(f)
            fmt = _read_u16_be(f)
            ntr = _read_u16_be(f)
            division = _read_u16_be(f)
            if hdr_len > 6:
                f.read(hdr_len - 6)
            if fmt != 1:
                raise ValueError("Expected Type-1, got %d" % fmt)
            if division & 0x8000:
                raise ValueError("SMPTE time division not supported")
            # ticks-per-quarter -> microseconds per tick, default tempo 120 bpm until meta tempo seen
            tick_us_ref[0] = int(60_000_000 / (120 * division))
            tracks = []
            for _ in range(ntr):
                if f.read(4) != b"MTrk":
                    raise ValueError("Missing MTrk")
                length = _read_u32_be(f)
                start = f.tell()
                f.seek(length, 1)
                tracks.append(TrackState(start, length))
        return tracks, division

    def _read_event(f, track: TrackState, tick_us_ref, ppq):
        if track.pos >= track.end:
            track.next_tick = None
            return
        f.seek(track.pos)
        delta = _read_varlen(f)
        if delta is None:
            track.next_tick = None
            return
        status_byte = f.read(1)
        if not status_byte:
            track.next_tick = None
            return
        status = status_byte[0]
        if status < 0x80:
            if track.running is None:
                track.next_tick = None
                return
            data1 = status
            status = track.running
        else:
            data1 = f.read(1)[0]
            track.running = status

        data2 = None
        is_meta = False
        if status == 0xFF:  # Meta
            is_meta = True
            meta_type = data1
            meta_len = _read_varlen(f) or 0
            meta_data = f.read(meta_len)
            if meta_type == 0x51 and meta_len == 3:
                mpq = (meta_data[0] << 16) | (meta_data[1] << 8) | meta_data[2]
                tick_us_ref[0] = int(mpq / ppq)
        elif status in (0xF0, 0xF7):
            syx_len = _read_varlen(f) or 0
            f.seek(syx_len, 1)
            is_meta = True
        else:
            status_hi = status & 0xF0
            if status_hi in (0xC0, 0xD0):
                pass  # 1 data byte; data1 already read
            else:
                data2 = f.read(1)[0]

        track.abs_tick += delta
        track.next_tick = track.abs_tick
        track.pos = f.tell()
        if is_meta:
            track.next_event = (None, None, None)
        else:
            track.next_event = (status, data1, data2)

    def stream_events(path):
        tick_us_ref = [0]
        tracks, ppq = _init_tracks(path, tick_us_ref)
        with open(path, "rb") as f:
            for trk in tracks:
                _read_event(f, trk, tick_us_ref, ppq)

            last_tick = 0
            while True:
                active = [t for t in tracks if t.next_tick is not None]
                if not active:
                    break
                next_trk = min(active, key=lambda t: t.next_tick)
                delta = next_trk.next_tick - last_tick
                last_tick = next_trk.next_tick
                status, data1, data2 = next_trk.next_event
                _read_event(f, next_trk, tick_us_ref, ppq)
                yield delta, status, data1, data2, tick_us_ref[0]

    while True:
        for delta, status, data1, data2, tick_us in stream_events(filename):
            if delta and tick_us > 0:
                # Use blocking sleep; precision is coarse but OK for playback
                time.sleep(delta * tick_us / 1_000_000)
            if status is None:
                continue
            status_hi = status & 0xF0
            if status_hi == 0x90:
                if data2 == 0:
                    midi.send(NoteOff(data1, velocity=0, channel=channel))
                else:
                    midi.send(NoteOn(data1, velocity=data2, channel=channel))
            elif status_hi == 0x80:
                midi.send(NoteOff(data1, velocity=data2 or 0, channel=channel))


def _is_dir(path: str) -> bool:
    # CircuitPython doesn't ship the 'stat' module; use the directory bit directly.
    _S_IFDIR = 0x4000
    try:
        mode = os.stat(path)[0]
        return bool(mode & _S_IFDIR)
    except OSError as exc:
        print(f"Skipping {path}: {exc}")
        return False


def _iter_midi_files(path: str):
    try:
        entries = sorted(os.listdir(path))
    except OSError as exc:
        print(f"Unable to list {path}: {exc}")
        return

    for name in entries:
        if name.startswith("."):  # skip macOS resource forks like ._foo.mid
            continue
        full_path = f"{path}/{name}"
        if _is_dir(full_path):
            yield from _iter_midi_files(full_path)
        elif name.lower().endswith(MIDI_EXTENSIONS):
            yield full_path


def _midi_format(path: str) -> int | None:
    """Return MIDI file format (0, 1, or 2) if header is valid, else None."""
    try:
        with open(path, "rb") as f:
            if f.read(4) != b"MThd":
                return None
            hdr_len = int.from_bytes(f.read(4), "big")
            if hdr_len < 4:
                return None
            fmt = int.from_bytes(f.read(2), "big")
            return fmt
    except OSError as exc:
        print(f"Cannot read {path}: {exc}")
        return None


def _ensure_mount_dir() -> None:
    import storage

    try:
        os.stat(MOUNT_POINT)
        return
    except OSError:
        pass

    # Try to make the mount directory. Root may be read-only if USB is connected.
    try:
        storage.remount("/", readonly=False)
    except Exception as exc:
        print("Could not remount root writable:", exc)
    try:
        os.mkdir(MOUNT_POINT)
    except OSError as exc:
        msg = (
            f"Mount point {MOUNT_POINT} missing and root is read-only "
            "while USB is connected. Disconnect CIRCUITPY drive and reset, "
            "or pre-create the directory once with a writable mount."
        )
        raise OSError(msg) from exc


def _mount_tf() -> str:
    import busio
    import displayio
    import sdcardio
    import storage

    from app.helpers import hardware

    sd_cs = board.GP13
    sd_sck = board.GP10
    sd_mosi = board.GP11
    sd_miso = board.GP12
    sd_baud = 24_000_000  # adjust down if your card or wiring needs slower SPI

    print(
        "Mounting TF card over SPI: CS=GP13, SCK=GP10, MOSI=GP11, MISO=GP12"
    )
    # If the board firmware pre-creates a display on this SPI bus, free it first.
    try:
        displayio.release_displays()
    except Exception as exc:
        print("release_displays failed (ignored):", exc)

    # Free pins in case a prior run or another module claimed them.
    for pin in (sd_cs, sd_sck, sd_mosi, sd_miso):
        try:
            hardware.release(pin)
        except Exception:
            pass

    spi = busio.SPI(clock=sd_sck, MOSI=sd_mosi, MISO=sd_miso)
    # sdcardio expects a Pin object, not a DigitalInOut
    sdcard = sdcardio.SDCard(spi, sd_cs, baudrate=sd_baud)
    vfs = storage.VfsFat(sdcard)
    _ensure_mount_dir()
    storage.mount(vfs, MOUNT_POINT)
    return MOUNT_POINT


def main_sd() -> None:
    mount_point = _mount_tf()
    for midi_path in _iter_midi_files(mount_point):
        fmt = _midi_format(midi_path)
        if fmt is None:
            print(f"Skipping non-MIDI file: {midi_path}")
            continue
        if fmt == 0:
            print(f"Streaming Type-0 MIDI file: {midi_path}")
            main2(filename=midi_path)
        elif fmt == 1:
            print(f"Streaming Type-1 MIDI file: {midi_path}")
            main_stream_type1(filename=midi_path)
        else:
            print(f"Skipping unsupported MIDI format (Type-{fmt}): {midi_path}")
        return  # main2/main_stream_type1 loop forever on success

    print("No valid .mid files found on the TF card.")
