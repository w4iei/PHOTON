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
`minmax` (live sensor state), `trace`/`capture` (waveforms), `flashtest`
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

The id persists in flash. The main controller board needs nothing.

## Console

Any node's USB-C gives a console (`screen /dev/tty.usbmodem* 115200`).
`help` lists commands: `stats`, `nodes`, `data`, `minmax`, `ping`, `trace`,
`capture`, `burst`, `cal save|reset`, `mode`, `disable`/`enable`, `setid`,
`flashtest`, `reboot`, `bootsel`, `id`.
USB-MIDI appears as "PHOTON Node" and emits notes when connected to the
bridge (main controller) board.

## Bench verification (M1–M5)

**M1 — dual-core / SRAM residency.** Flash any board, connect the console:
`stats` shows the sweep counter advancing (sensor board). Run `flashtest`:
it erases/programs flash 10× **without pausing core 1** and prints sweep
timing before/after — sweep_us must stay flat. For scope proof, watch any
bank CS line: its cadence must not flinch during `flashtest`.

**M2 — scan parity + rate.** On a sensor board, run the unmodified capture
tool against a key press:
`python software/host_code/listen_for_single_sensor_high_res.py`
— its `capture <seconds>` trigger is handled natively (select the sensor
first with `trace <sensor>` once, or use the default). Compare the
curve and ON/OFF crossings against a CircuitPython-build capture of the same
key. Check `stats`: `sweep_us` ≤ 1000 (≥1 kHz) in mode 1. Then run the
**crosstalk experiment**: with the array over a static surface, capture
`minmax` in `mode 0` (sequential) vs `mode 1` (parallel); per-sensor deltas
must stay under 1% of that sensor's range — if not, use `mode 2` (two-phase).

**M3 — transport soak.** Main board + ≥2 sensor nodes on the bus. From the
bridge console: `burst 32` repeatedly (or scripted over CDC). `stats <id>`
on each node shows events_on/off totals; `nodes` on the bridge shows
events received per node — totals must match exactly (zero loss), with
crc/hdr error counters explaining any retries. Wiggle/unplug a bus cable
mid-flood: the node goes SILENT, is re-discovered on reconnect, and
counters stay consistent.

**M4 — end-to-end MIDI.** Bridge board USB into a DAW: play keys, notes
sound with velocity. `cal save` persists calibration on all nodes; power
cycle and confirm notes still work without re-learning.

**M5 — chord-burst regression (the motivating bug).** Script over the
bridge CDC: `burst 32` × 10⁴ iterations; after each, compare node
events_on/off sums against bridge events_rx (`nodes`). Pass = zero lost
events and no ring overflows. Finale: fast two-hand block chords on the
real instrument against an audio recording — every note present.

## Host unit tests (no hardware)

```bash
cd firmware/test/host
cmake -B build -G Ninja && ninja -C build && ctest --test-dir build
```

Covers: frame codec (CRC vectors, roundtrip, per-byte corruption rejection,
streaming fuzz with 100% recovery) and the event engine (dt math,
hysteresis, range gate, boot-disable) under ASan/UBSan.
