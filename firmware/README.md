# PHOTON Native Firmware

Bare-metal C (Pico SDK) firmware for all PHOTON boards — replaces the
CircuitPython stack. One UF2 for every board: at boot the firmware probes the
TLA2518 banks; a board with sensors becomes a **sensor node** (core 1 runs the
SRAM-resident scan/event loop), a board without (the main controller board)
becomes the **bus master/bridge** (RS-485 poll cycle + USB-MIDI + console).

Design details: [docs/architecture/01-native-dual-core-firmware.md](../docs/architecture/01-native-dual-core-firmware.md).
Legacy CircuitPython stack (kept as parity reference): `../software/embedded_software/`.

## Build

Requirements: `arm-none-eabi-gcc`, `cmake`, `ninja`, `picotool` ≥ 2.3
(all in Homebrew), and a pico-sdk ≥ 2.1 checkout with the `lib/tinyusb`
submodule initialized:

```bash
git clone --depth 1 --branch 2.3.0 https://github.com/raspberrypi/pico-sdk.git ~/pico-sdk
git -C ~/pico-sdk submodule update --init --depth 1 lib/tinyusb
```

Then:

```bash
cd firmware
cmake -B build -G Ninja        # set -DPICO_SDK_PATH=... if not ~/pico-sdk
ninja -C build                 # -> build/photon.uf2
```

The image is built **copy-to-RAM**: all code executes from SRAM, so core-0
flash writes (calibration saves) can never stall the core-1 scan loop.

## Flash / update

First time (blank board): hold **USB-BOOT** while connecting USB-C (or short
the USB-BOOT jumper), then:

```bash
picotool load -f build/photon.uf2 && picotool reboot
```

or copy `photon.uf2` onto the `RP2350` drive.

Updating a running board never needs the button: type `bootsel` on its
console (or `bootsel <id>` from the bridge for a remote node), then run the
same `picotool` line. `tools/flash_all.sh` does this for the whole system.

**Everything configured survives a reflash**: node id, calibration, disabled
masks, channel map, scan mode/rate, velocity range. Config lives in two
sectors above the program image, which a UF2 drop does not touch, and the
loader accepts records written by any older build (fields are append-only,
so an old record is a prefix of the current one; missing fields take
defaults). A migrated board says so once in its boot banner and `id`, then
rewrites the record in the current layout. Check with `disable` (no
argument, lists the masks), `chmap` and `velrange` after flashing. The only
way to lose settings is `cal reset` / `setid`.

## Validated settings

These are the compiled defaults, measured on the four-board two-manual
instrument (2026-09-02). Every one is also a runtime knob, persisted in
flash.

| setting | value | why |
|---|---|---|
| scan mode | 2, two-phase (`mode`) | half the peak emitter current of parallel, 8-key spacing between lit emitters |
| scan rate | 600 Hz (`rate`) | 1.67 ms velocity quantisation, 55 steps across the playing range; sweep 969 µs so ~40% idle |
| emitter settle | 50 µs (`settle`) | more changes nothing measurable |
| SPI bus | 10 MHz | faster clocks lose register writes to the ADC nearest the MCU (near-end reflection on the 414 mm board) |
| velocity map | log in dt, 2.5–25 ms → 50–120, gamma 2 (`velcurve`, `velrange`) | pp / mf-f / ff strikes measure 24.9 / 9.9 / 3.3 ms median, a constant ratio apart; lands them at 54 / 95 / 119 |
| system power | 1.08 W at 5 V, all four boards | measured at the USB meter; mode and rate move it by ~0.1 W per 100 Hz only |

Mode 0 (sequential, ~390 Hz ceiling) lights one emitter at a time and is
kept only as a crosstalk-free benchmarking reference.

One hardware quirk the driver works around: on every TLA2518 fitted, GPIO0
in push-pull mode drives high regardless of GPO_VALUE, so the slot-3
emitter of each bank is switched through its drive-mode bit instead
(`tla2518_emitters()`). Any board flashed with an older build had those
seven emitters on continuously and must be recalibrated after updating.

## Debugging (USB-only)

The current boards have no SWD connector populated, so **the USB console is
the debugging interface**: `stats` (rates, counters, error tallies), `data`/
`minmax` (live sensor state), `trace`/`capture` (per-sweep waveforms, local
or via the bridge with `trace <sensor> <node>`), `flashtest`
(core-1 independence proof), plus the `# LOG`/`# NOTE` diagnostic stream. A
sensor node whose array reads all-zero at boot deliberately *suppresses* its
auto-reboot recovery while a console is attached, so the fault can be
inspected instead of loop-rebooting. (If SWD headers are ever soldered on,
standard OpenOCD + Debug Probe works: `openocd -f interface/cmsis-dap.cfg -f
target/rp2350.cfg`.)

## First-time setup per sensor node

Each sensor node needs a bus id once (replaces the CIRCUITPY
`/sensor_node_id` file). Connect USB to the node and in a serial terminal
(115200, any rate — CDC):

```
setid 1        # 1..6, unique per node
```

then calibrate at the operating scan rate: `r`, play every key once at normal
force, `s`. Id and calibration persist in flash. On the bridge, `chmap <manual>
<channel>` maps each manual (board pair) to its MIDI channel and `disable
<global idx>` masks unpopulated slots on remote boards.

## Console

Any node's USB-C gives a console (`screen /dev/tty.usbmodem* 115200`).
`help` lists commands: `stats`, `nodes`, `data`, `minmax`, `ping`, `trace`,
`capture`, `burst`, `test` (pseudorandom load), `cal save|reset`, `r`/`s`/`x`
(calibration), `mode`, `rate`, `localmidi`, `chmap`, `disable`/`enable`,
`velrange`, `velcurve`, `setid`, `log on|off`, `flashtest`, `sd`, `reboot`,
`bootsel`, `id`.
USB-MIDI appears as "PHOTON Node" and emits notes when connected to the
bridge (main controller) board.

## microSD recorder (bridge)

Put a microSD card in the main controller board's socket and forget about
it: everything the bridge emits as MIDI is also written to the card as
Standard MIDI Files, whether or not a host is listening.

```
0001/            one directory per power-on, created on the first note
0001/0001.MID    one file per playing episode
0001/0002.MID    ... opened on the first note, closed after 30 s of silence
0002/            next power-on
```

- **Numbering is the only bookkeeping.** At mount the bridge scans the root
  for the highest `NNNN` directory and continues from there. Directories are
  created lazily, so idle power cycles leave nothing behind. At 9999
  (directories or files) the recorder stops; it never wraps or overwrites.
- **No clock, no dates.** The board has no RTC and USB carries no time, so
  every file opens with a text meta event `PHOTON power-on +HH:MM:SS.mmm`
  (time since power-on). Inside a file the delta times are exact
  milliseconds (SMPTE 25 fps × 40 ticks division), no tempo map.
- **Power-cut safe.** Every 500 ms the pending events are written and the
  file is re-terminated (end-of-track plus the real track length), so the
  card always holds a complete, valid file. A yanked cable loses at most the
  last half second.
- **A held key delays the close.** The 30 s silence close waits for every
  note to be released, capped at 5 minutes.
- **Cards:** FAT16, FAT32 and exFAT (a 64 GB card as sold). No card, a
  pulled card, or a card error just means a retry every 2 s; nothing
  else on the bridge notices.
- **Latency:** the recorder runs on the bridge's otherwise idle core 1 with
  its own SPI bus (SPI1: SCK 10, MOSI 11, MISO 12, CS 13), so card stalls
  never touch the RS-485 poll cycle or USB-MIDI. Core 0 hands over messages
  through a 512-deep ring (`drops` in the status line counts overflow).

Console: `sd` prints the status line (state, card size and free space,
current directory/file, counters, last error); `sd test [n]` plays a scale
through the MIDI output on a bare bridge so the recorder can be exercised
without sensor boards. A connected terminal also gets one line per state
change (`[SD] recording 0001/0003.MID`, `[SD] closed ...`, `[SD] no card`).

## Bench verification (M1–M5): results

All five milestones passed on the real four-board, two-manual system.

**M1 — SRAM residency.** `flashtest` hammers flash 10× while core 1 scans:
sweep timing stays flat. Verified.

**M2 — scan rate.** Full 31-sensor sweep measured at 766 µs in parallel
mode (mode 1) and 969 µs two-phase (mode 2, the default) at the validated
10 MHz SPI clock; production paces mode 2 at 600 Hz (`rate`), which also
sets the calibration operating point. Verified.

**M3 — transport soak.** Bus tuned to 4 Mbaud / 8 µs DE guards / 700 µs poll
timeout: 3,694 polls/s per node (2 nodes), ~1,540/s (4 nodes). 21-minute soak
at 100 ev/s/node: 246k events, 7.2M frames, crc_err=0 gaps=0 dup=0; 500
ev/s/node × 60 s equally clean. Verified.

**M4 — end-to-end MIDI.** Four boards through the bridge into a DAW, top
manual on channel 3, lower on channel 2 (`chmap`); calibration survives power
cycles. Verified.

**M5 — chord-burst regression (the motivating bug).** Generator loads far
beyond human playing rates show zero sequence-accounted loss; real two-hand
block chords on the instrument: every note present. Verified.

To re-run any of these, the commands above (`flashtest`, `stats`, `test`,
`nodes`, `cal save`) reproduce the numbers from the bridge console alone.

## Host unit tests (no hardware)

```bash
cd firmware/test/host
cmake -B build -G Ninja && ninja -C build && ctest --test-dir build
```

Covers: frame codec (CRC vectors, roundtrip, per-byte corruption rejection,
streaming fuzz with 100% recovery), the event engine (dt math, hysteresis,
range gate, boot-disable), and the microSD recorder (the production
recorder + FatFs on a RAM disk formatted FAT16/FAT32/exFAT: numbering across
power cycles, flush validity, silence close, held-note cap, ring overflow,
card errors, late card insert, the 9999 stop) under ASan/UBSan.
