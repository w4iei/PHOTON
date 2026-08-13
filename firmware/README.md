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

## Flash

First time (blank board): hold **USB-BOOT** while connecting USB-C (or short
the USB-BOOT jumper), then:

```bash
picotool load -f build/photon.uf2 && picotool reboot
```

or copy `photon.uf2` onto the `RP2350` drive. Once this firmware is running,
the button is never needed again: the console command `bootsel` re-enters the
UF2 bootloader and `reboot` restarts the firmware — reflash cycles are fully
USB-driven.

## Debugging (USB-only)

The current boards have no SWD connector populated, so **the USB console is
the debugging interface**: `stats` (rates, counters, error tallies), `data`/
`minmax` (live sensor state), `trace`/`capture` (waveforms; the one console
path not exercised during bring-up — treat as unverified), `flashtest`
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
`setid`, `log on|off`, `flashtest`, `reboot`, `bootsel`, `id`.
USB-MIDI appears as "PHOTON Node" and emits notes when connected to the
bridge (main controller) board.

## Bench verification (M1–M5): results

All five milestones passed on the real four-board, two-manual system.

**M1 — SRAM residency.** `flashtest` hammers flash 10× while core 1 scans:
sweep timing stays flat. Verified.

**M2 — scan rate.** Full 31-sensor sweep measured at 592 µs (~1.7 kHz
open-loop) in parallel mode (mode 1, the default); production paces the sweep
at 400 Hz (`rate`), which also sets the calibration operating point. Verified
(except the `trace`/`capture` host-tool path — unexercised).

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
streaming fuzz with 100% recovery) and the event engine (dt math,
hysteresis, range gate, boot-disable) under ASan/UBSan.
