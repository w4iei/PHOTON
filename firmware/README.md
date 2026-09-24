# PHOTON Native Firmware

Bare-metal C (Pico SDK) firmware for all PHOTON boards. One UF2 for every
board: at boot the firmware probes the
TLA2518 banks; a board with sensors becomes a **sensor node** (core 1 runs the
SRAM-resident scan/event loop), a board without (the main controller board)
becomes the **bus master/bridge** (RS-485 poll cycle + USB-MIDI + console).
An instrument can also be built from sensor boards alone: with `master on`
(saved) on each board, whichever carries the USB cable runs the bus as well,
see [Without a main controller board](#without-a-main-controller-board).

Design details: [docs/architecture/01-native-dual-core-firmware.md](../docs/architecture/01-native-dual-core-firmware.md).

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
or via the bridge with `trace <sensor> <node>`; `tools/listen_for_single_sensor_high_res.py`
captures and plots them on the host), `flashtest`
(core-1 independence proof), plus the `# LOG`/`# NOTE` diagnostic stream. A
sensor node whose array reads all-zero at boot deliberately *suppresses* its
auto-reboot recovery while a console is attached, so the fault can be
inspected instead of loop-rebooting. (If SWD headers are ever soldered on,
standard OpenOCD + Debug Probe works: `openocd -f interface/cmsis-dap.cfg -f
target/rp2350.cfg`.)

## First-time setup per sensor node

Each sensor node needs a bus id once. Connect USB to the node and in a
serial terminal (115200, any rate — CDC):

```
setid 1        # 1..6, unique per node
```

then calibrate at the operating scan rate, as described under
[Calibration](#calibration): `r`, slow-press every key until it plucks, `s`.
Id and calibration persist in flash. On the bridge, `chmap <manual>
<channel>` maps each manual (board pair) to its MIDI channel and `disable
<global idx>` masks unpopulated slots on remote boards.

## Calibration

Calibration learns each key's travel (min and max) and finds the point in
that travel where the key plucks. A fixed strike threshold at 60% of the
travel sits at a different place relative to each key's pluck. On a recorded
MIDI+audio corpus it fired 8-60 ms before the pluck on slow presses and a few ms
after it on fast ones, with a different offset per key. So calibration
captures every key's swing and saves a strike threshold per key, at its
pluck.

The plectrum loads the string as the key goes down: the key slows or creeps,
for 0.2-1 s on a slow press, while the finger's force builds. When the
plectrum lets go the load vanishes and the key snaps down. That knee in the
position trace is the pluck. It is searched within 40-80% of the travel
(the 60% threshold ± 20%). The snap builds up over 5-13 ms, not in one
sample: it must be 2.5 times steeper, over 8 ms, than both the approach
over the preceding 20 ms and its last 10 ms, and carry 8% of the travel.
If the snap already started below the window, the key has no knee in it
(the pluck is not reported at the window's edge). The key's threshold is
saved 3% above the knee, so the creep while the string is loaded cannot
reach it early. Each swing is captured for up to 3.4 s, from just before
the key starts down, and ends once the key is back within a fifth of its
depth: keys settle hundreds to thousands of counts off where they started,
and that level becomes their rest.

The test instrument's upper manual, calibrated this way on 2026-09-24 (61/61 keys),
plucks between 41% and 77% of the travel, with thresholds from 44% to 80%.
In the bass several keys climb in two or three steps before the final snap
(more than one register?); the first knee inside the window is the one
taken.

From the bridge console (or a board's own, with `r` / `s` / `x`):

```
cal reset 1 2      # boards 1 and 2 start learning; the live view opens
                   # slow-press every key until it plucks, then let it up
cal save 1 2       # store; the before/after table follows
```

- **Coupler off.** With the coupler engaged, the other manual's key moves too
  and the swing carries two plucks. A key seen moving with its twin on the
  other manual is marked `C`.
- **Slow presses.** A fast stroke is usually steep throughout, with no knee
  to find (marked `?`: press it again, slower). Once a key is green, a later
  bad press does not undo it. Each key keeps up to four good knees; the
  median is used.
- **Keys that are not neighbours may go down together**, so a hand can take
  several at once. Neighbours pressed together light each other's sensors,
  so both are marked `A` and redone one at a time.
- **The live view** shows each manual being calibrated as a keyboard, with
  black keys raised: `+` green (pluck captured), `.` not yet, `?` / `A` / `C`
  redo, with the reason listed under the keyboard. `cal view` toggles it.
- **Before/after:** `cal compare [id ...]` prints, per key, the min, max and
  strike threshold (% of travel and in counts) that were in force when
  calibration started, next to the new ones. `cal save` prints it itself.
  A `g` marks the global 60%.
- **The swings themselves** stay on the boards until the next calibration:
  `cal dump <id> [key]` prints them, and `tools/cal_dump.py --port <tty>
  --boards 1 2 --plot` saves every key's swing as CSV, with the before/after
  tables and a plot per board.
- A key without a knee at save time keeps the global 60%. `cal save` on a
  board that was not calibrating leaves its thresholds alone. Configs from
  older builds load with every key at 60%.

**Tuning the rules.** `cal rules <window> <ratio> <jump> <margin> [id ...]`
sets the search window (± % around 60), how many times steeper the snap must
be than the approach, how much of the travel it must carry, and the
threshold's margin above the knee. `0` for any of them is the default: `cal
rules 20 2.5 8 3`, printed by `cal rules`. The rules are saved on each
board. A calibration in progress there is judged again from each key's last
swing, one key per pass of the main loop (so the board keeps answering the
bus); re-judging finds new knees but never turns a green key back.
The defaults were tuned on 34 swings of the test instrument's upper manual and checked
against the January 2026 single-sensor traces: they find the knee in 32 of
the 34 (the other two show no pluck), in all four slow January presses,
and nothing in the two January traces with the action disengaged (no
string, no pluck). A window of ±30 finds a false knee in one of those; keep
it at 20 unless a manual plucks lower. Save a manual's swings with `cal
dump` (or `tools/cal_dump.py`) when tuning.

### Strike mode: knee or global

`strike knee|global [id ...]` (saved per board; no ids = every board):

- **knee** (default): each key strikes at its own threshold from knee
  calibration; keys without one use 60%.
- **global**: every key strikes at 60%, the fixed threshold used before
  knee calibration. Use it for an instrument without a pluck (an organ, say),
  or as a fallback. Calibration then asks for no knees: any full press turns
  a key green, as in the old flow. Knees found along the way are still saved,
  so switching back to `strike knee` needs no recalibration.

`strike` alone shows the mode; the banner shows it too.

## Without a main controller board

Two sensor boards make a whole single-manual instrument on their own. Cable
them together on RS-485 (termination switches on at both ends), give them
ids 1 and 2, and on each board's console once:

```
master on      # saved
```

From then on whichever board carries the USB cable runs the bus: about a
second after the host has enumerated it, having heard nothing on the wire,
it starts polling the other board and owns USB-MIDI and the console exactly
as the main controller board would (`id` says `role=master+node`). Its own
keys stay in the note map under its node id, so node 1 is always the low
half of the manual and node 2 the high half, whichever of them has the
cable. The chain is powered through that cable, so moving it power-cycles
both boards and the roles follow it. A `master on` board that does hear a
master (the other board already running the bus, or a main controller board)
stays a plain node. Every bridge-form command works from the master. `data`, `minmax` and `trace` read
the local scanner when the id is omitted or is the board's own, and a remote
node otherwise; the bus-wide forms of `cal`, `mode`, `rate`, `settle`,
`burst` and `test` (no id) reach every node and the local scanner alike;
`disable`/`enable` take global sensor indexes, as on the bridge. `nodes`
lists the board itself first. A sensor board has no microSD socket, so
nothing is recorded in this configuration.

Never put `master on` boards on the same bus as a main controller board: a
board with a USB host would claim the bus while the main board is still
booting, and two masters collide. A master that hears frames from bus
address 0 logs a warning once and counts them in `stats`
(`other_master_frames`). `master off` returns a board to a plain node at
the next power-up.

## Console

Any node's USB-C gives a console (`screen /dev/tty.usbmodem* 115200`).
`help` lists commands: `stats`, `nodes`, `data`, `minmax`, `ping`, `trace`,
`capture`, `burst`, `test` (pseudorandom load),
`cal reset|save|view|compare|dump|rules`, `r`/`s`/`x` (calibration), `strike`,
`mode`, `rate`, `localmidi`, `master`, `chmap`,
`disable`/`enable`, `velrange`, `velcurve`, `setid`, `log on|off`,
`flashtest`, `sd`, `reboot`, `bootsel`, `id`.
The banner and `id` name the build: git short hash (`-dirty` when `firmware/`
had uncommitted changes) and build time, from a `build_id.h` that CMake
regenerates on every build. `picotool info -a build/photon.uf2`, or a board
sitting in BOOTSEL, shows the same as *version* / *build date*, next to the
repository and klavecimbel.com.
USB-MIDI appears as "PHOTON Node" and emits notes when connected to the
board that runs the bus (the main controller board, or a `master on` sensor
board).

## microSD recorder (bridge)

Put a microSD card in the main controller board's socket and forget about
it: everything the bridge emits as MIDI is also written to the card as
Standard MIDI Files, whether or not a host is listening.

```
0001/            one directory per power-on, created on the first note
0001/0001.MID    one file per playing episode
0001/0002.MID    ... opened on the first note, closed after 30 s of silence
0001/SETUP.TXT   the settings and calibration this power-on played with
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
- **SETUP.TXT: what the recordings were played with.** Three seconds after
  power-on the bridge reads every board's calibration table (the one
  `cal compare` shows: min, max and strike threshold per key, strike mode)
  and adds its own build, serial, channel map and velocity settings. The
  text goes into the power-on's directory once it exists. It is read once
  per power-on: a change made later shows up in the next power-on's file.
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
change (`[SD] recording 0001/0003.MID`, `[SD] closed ...`, `[SD] no card`,
`[SD] saved 0001/SETUP.TXT`).

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
range gate, boot-disable, per-key strike thresholds), knee calibration (the
detector on synthetic slow presses and their look-alikes: fast strokes, a
finger speeding up, a slipping plectrum, a one-sample glitch; the core-1
swing capture; the session's statuses, neighbour rule, crosstalk rejection,
save and wire payloads), and the microSD recorder (the production
recorder + FatFs on a RAM disk formatted FAT16/FAT32/exFAT: numbering across
power cycles, flush validity, silence close, held-note cap, ring overflow,
card errors, late card insert, the 9999 stop) under ASan/UBSan.
