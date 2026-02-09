"""Mount TF card and write a random 30s MIDI file to /sd/1.mid."""

from __future__ import annotations

import os
import random
import time

import board
import busio
import displayio
import sdcardio
import storage

SD_CS = board.GP13
SD_SCK = board.GP10
SD_MOSI = board.GP11
SD_MISO = board.GP12
MOUNT_POINT = "/sd"
SD_BAUD = 24_000_000

_S_IFDIR = 0x4000


def _ensure_mount_dir() -> None:
    try:
        mode = os.stat(MOUNT_POINT)[0]
        if mode & _S_IFDIR:
            return
    except OSError:
        pass
    try:
        storage.remount("/", readonly=False)
    except Exception as exc:
        print("Could not remount root writable:", exc)
    try:
        os.mkdir(MOUNT_POINT)
    except OSError as exc:
        raise OSError(
            f"Mount point {MOUNT_POINT} missing and root is read-only. "
            "Create it once (eject CIRCUITPY or use deploy script) and retry."
        ) from exc


def _mount_tf() -> str:
    print("Mounting TF card over SPI: CS=GP13, SCK=GP10, MOSI=GP11, MISO=GP12")
    try:
        displayio.release_displays()
    except Exception:
        pass

    spi = busio.SPI(clock=SD_SCK, MOSI=SD_MOSI, MISO=SD_MISO)
    sd = sdcardio.SDCard(spi, SD_CS, baudrate=SD_BAUD)
    vfs = storage.VfsFat(sd)
    _ensure_mount_dir()
    storage.mount(vfs, MOUNT_POINT)
    return MOUNT_POINT


def _varlen(value: int) -> bytes:
    """Encode a value as a MIDI variable-length quantity."""
    buf = value & 0x7F
    out = []
    while True:
        out.insert(0, buf)
        value >>= 7
        if not value:
            break
        buf = (value & 0x7F) | 0x80
    return bytes(out)


def _build_track(duration_s=30, bpm=120, ppq=96):
    total_ticks = int(duration_s * (bpm / 60) * ppq)
    ticks_left = total_ticks
    track = bytearray()

    last_tick = 0
    note_on_count = 0
    while ticks_left > 0:
        step = random.randint(ppq // 8, ppq // 2)  # between 32nd and 8th note
        if step > ticks_left:
            step = ticks_left
        ticks_left -= step
        delta = step
        note = random.randint(48, 72)  # C3..C5-ish
        vel = random.randint(60, 110)
        dur = max(ppq // 8, min(ppq, int(step * 1.5)))

        # Note on
        track.extend(_varlen(delta))
        track.extend((0x90, note & 0x7F, vel & 0x7F))
        note_on_count += 1
        # Note off after dur ticks
        track.extend(_varlen(dur))
        track.extend((0x80, note & 0x7F, 0))
        last_tick += delta + dur

    # End-of-track
    track.extend(_varlen(0))
    track.extend((0xFF, 0x2F, 0x00))
    return bytes(track), note_on_count


def _build_mid_file(track_data: bytes, ppq=96) -> bytes:
    header = bytearray()
    header.extend(b"MThd")
    header.extend((0, 0, 0, 6))  # length
    header.extend((0, 0))  # format 0
    header.extend((0, 1))  # one track
    header.extend(((ppq >> 8) & 0xFF, ppq & 0xFF))

    track_chunk = bytearray()
    track_chunk.extend(b"MTrk")
    track_chunk.extend(len(track_data).to_bytes(4, "big"))
    track_chunk.extend(track_data)
    return bytes(header + track_chunk)


def main() -> None:
    mount_point = _mount_tf()
    target = f"{mount_point}/1.mid"

    print("Building random MIDI data...")
    track, note_on_count = _build_track()
    midi_bytes = _build_mid_file(track)
    print("MIDI bytes:", len(midi_bytes))

    t_open = time.monotonic()
    with open(target, "wb") as f:
        t_after_open = time.monotonic()
        f.write(midi_bytes)
        t_after_write = time.monotonic()
    t_after_close = time.monotonic()

    print(f"Wrote {len(midi_bytes)} bytes to {target}")
    print(f"Note-on events: {note_on_count} (note-off paired for each)")
    print(f"SD baud rate: {SD_BAUD/1_000_000:.2f} MHz")
    print(f"Open time:  {(t_after_open - t_open)*1000:.1f} ms")
    print(f"Write time: {(t_after_write - t_after_open)*1000:.1f} ms")
    print(f"Close time: {(t_after_close - t_after_write)*1000:.1f} ms")


if __name__ == "__main__":
    main()
