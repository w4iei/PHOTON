# Next Bench Session — Plan

Short and ordered.

Step 2 is software-only and testable on the host. Stored config now survives
it (see 2a: older records migrate by prefix scan), so no `setid` or
recalibration pass is needed. Everything else needs the hardware connected.

## 1. Power characterisation + RS-485 sanity (~15 min) — DONE 2026-09-02

Results and interpretation are in `03-scan-modes.md` ("Rev 1D
re-measurement"). Outcome: floor 3.02 W, mode invisible, rate ~0.1 W per
100 Hz; transport clean. **Production moved to mode 2 at 600 Hz** (compiled
default and deployed to all four nodes; existing calibration still brackets
the rest readings, no recalibration needed). Step 3 remains.

Now that main rev 1D uses a buck, re-measure with a USB power meter at the
5 V input. Set each config from the bridge console (`mode <m> <id>`,
`rate <hz> <id>`, all four nodes), let it settle ~10 s, read the meter:

| mode | rate | note |
|---|---|---|
| 2 two-phase | 300 Hz | production config — the baseline |
| 2 two-phase | 600 Hz | does rate matter at all? (expect: no) |
| 1 parallel | 300 Hz | 2x peak emitter current |
| 1 parallel | 400 Hz | old `full_performance` operating point |

Expected: still dominated by fixed baseline (~2.4 W delivered), with mode/rate
worth only a few hundred mW. Mode 0 is deliberately absent — it is a
benchmarking instrument rather than a performance mode, and `03-scan-modes.md`
already establishes that mode is not a power lever. It gets its own pass in
step 3.

Not pursuing further power work. For the record, in case it comes up again:
core 1 busy-waits (`wait_until_us` -> `busy_wait_us_32`) through its ~76% idle
gap rather than sleeping, so a WFE/timer wait is available. It is NOT worth
doing — datasheet 14.9.7.3 puts a single RP2350 core at 150 MHz at ~39 mW for
the whole chip, so halting core 1 on four boards saves roughly 0.08 W of 2.7 W
(~3%). Note also that component estimates do not account for ~500 mA of the
3.3 V rail; if power ever matters again, measure the per-board increment
(bridge alone, then +1/+2/+3/+4 boards) rather than modelling it.

Then RS-485, 20 s only (nothing in the transport changed):
```
test 100          # broadcast load
# wait 20 s
nodes             # expect gaps=0 malformed=0 dup=0, ~1000 polls/s/node
stats             # expect crc_err=0 hdr_err=0
test stop
```
This is also the first real traffic over the rev 1D board's transceiver.

## 2. Velocity mapping for harpsichord (the substantive item)

**Problem.** A harpsichord's loudness is essentially independent of touch —
the plectrum plucks the same way regardless of key speed. But we emit MIDI
velocity 1-127, and a piano sample library treats velocity 1 as "barely
sound the note". Users on piano libraries hear notes that don't play.

**Change.** Keep the dt curve, compress its *output* range. Today:
```c
v = 1.0f + 126.0f * powf(x, vel_curve);      // 1..127
```
becomes
```c
v = out_min + (out_max - out_min) * powf(x, vel_curve);
```
with `out_min`/`out_max` defaulting to **75 / 115** — always clearly audible,
still expressive, never maxed.

**Make it a runtime knob, not a constant.** "What number is right" is a taste
question the user will iterate on: add `velrange <min> <max>` to the console
(local + remote via NODECTL, persisted), so it can be tuned against a real
sample library without reflashing.

Velocity is computed wherever MIDI is emitted — events cross the wire as raw
`dt_us` and are mapped by the receiver. Normally that is the bridge alone; a
node with `localmidi on` maps its own. Neither the frame format nor NODECTL
(op + u16 arg) changes, so a new-firmware bridge interoperates with
old-firmware nodes.

### 2a. Config layout: append-only, migrated by prefix scan (revised)

Appending two floats to `photon_config_t` lengthens the record, and the CRC32
sits at the end. The earlier decision here was to take a one-time reset
rather than carry a table of historical record sizes. Superseded on
2026-09-02: the deployed bridge must not lose its masks or channel map on
*any* reflash, and it turns out no table is needed.

Because fields are only ever appended, a record written by any older build is
a byte-for-byte prefix of the current struct with its CRC in its last four
bytes. `config_store_init()` first checks the current length, then scans
prefix lengths from `PHOTON_CONFIG_MIN_RECORD` up; on a match it overlays that
prefix on compiled defaults, so the fields the old record lacks take their
defaults, and saves once in the current layout. Cost: ~130 CRCs over ≤200
bytes at boot, only on a stale record. Verified on the bridge: a 191-byte v5
record migrated with masks and `chmap` intact, `velrange` at its 75-115
default.

The rule this buys: **append only**, never reorder/resize/remove a field.
`load_defaults()` remains the single place new fields get their values.

### 2b. What the reset costs, and how it comes back

| Field | Recovery |
|---|---|
| `cal_min[32]` / `cal_max[32]` | Automatic — the event engine tracks running min/max. Play every key through full travel, then `cal save` |
| `node_id` | `setid N` per board |
| `global_disabled`, `manual_channel`, midi low/high/channel | Re-enter from the console, only if customised off the defaults |

Calibration is the bulk of the record and the cheapest part to recover: it
regenerates by playing the instrument, and `cal reset [id]` / `cal save [id]`
both work remotely from the bridge, so it needs no per-board USB.

The one sharp edge is node identity. Every board returns as `node_id = 1`, all
answering at the same bus address, and NODECTL `setid` is addressed-only — so
it cannot be fixed over the wire once they collide. Flashing a UF2 already
requires physical USB in BOOTSEL, so **run `setid N` in the same session as
the flash, before the board goes back on the bus.**

Boards self-report: `config_store_init()` sets `g_config_from_flash = false`
and the console banner prints `(defaults, uncalibrated)`, so a board that got
missed announces itself the moment you attach. That is the whole guard, and it
already exists.

### 2c. Then: the velocity change itself — DONE

- `vel_out_min` / `vel_out_max` appended to `photon_config_t` (now 50/120)
- `midi_map_velocity()` uses the compressed range — and, since 2d, a log window
- `velrange <min> <max>` console command, local + NODECTL, persisted
- older config records migrate (2a), nothing is reset

### 2d. Velocity map: plan of record (2026-09-02)

Measured on the instrument at 600 Hz, three passes by the player, note-on dt
captured from the bridge's `[EVT]` stream (raw data:
`data/velocity-strikes-2026-09-02.tsv`, 1,587 strikes, both manuals, coupled
and uncoupled):

| dynamic | strikes | dt p10 | **p50** | p90 | old curve emitted |
|---|---|---|---|---|---|
| pp | 24 | 4.9 ms | **24.9** | 56.6 | 100 (29% at 115) |
| mf-f | 367 | 3.3 | **9.9** | 31.6 | 112 |
| ff | 1,196 | 1.6 | **3.3** | 8.3 | 115 (85% at 115) |

Two findings. **Each dynamic is ~2.5x the next in dt**, so the natural axis
is log(dt), not dt. And **the whole playing range sits below 8 ms** — the
inherited 8-100 ms window started where real playing ends, which is why
everything read 112-115.

Per key the spread was large (the same pp touch read 3 ms on G4 and 60 ms
on E4). The fast keys were exactly the slot-3 sensors, whose emitters were
permanently on (see `03-scan-modes.md`); re-fit after that fix.

**The map** (`midi_map_velocity()`, defaults in `board_config.h`):

    x = log(dt / min_ms) / log(max_ms / min_ms)      clamped to 0..1
    v = out_max - (out_max - out_min) * x^gamma

with **min 2.5 ms, max 25 ms, gamma 2, output 50-120**. Simulated on the
captured sets:

| dynamic | median | p10-p90 |
|---|---|---|
| pp | 54 | 50-114 |
| mf-f | 95 | 50-119 |
| ff | 119 | 101-120 |

Gamma > 1 is the skew that holds mf-f up at 95 (a plain log put it at 78).
Per scan period at 600 Hz the ladder is 120 120 119 114 107 101 95 89 83
78 72 68 63 58 54 50 — ~6 velocity units per step through the playing
range. At 300 Hz it would be 120 114 95 83 72 63 57 52 50: coarser
everywhere, and anything under 3.3 ms (rather than 1.7) pins at 120.
That is the only cost of a lower rate.

Known and accepted: 56% of ff strikes cross the 30%-60% window within two
scans and pin at 119-120, so f and ff are not separated. Sub-period
interpolation on the node (crossing time between the samples either side of
each threshold) would recover that without a rate change; not planned
unless it is wanted musically.

Runtime: `velrange <min> <max>` (output band) and `velcurve <min_ms>
<max_ms> <gamma>` (window + skew), both persisted on the bridge and
broadcast (NODECTL ops 6 and 7/8/9) so every board holds the same map.
Deployed to the bridge and set over the console; the persisted values are
what runs, the compiled defaults match them for fresh boards.

### 2e. Next session, first

All four nodes run the 2026-09-02 build (GPIO0 emitter fix, 10 MHz SPI; see
`03-scan-modes.md`). Recalibrate all four boards from the bridge
(`cal reset <id>`, play, `cal save <id>`), re-capture the pp/mf/ff strike
sets and re-fit the velocity curve (2d), then step 3.

## 3. Mode 0 characterisation (benchmarking, not deployment)

Mode 0 lights one emitter at a time, so it is the only mode free of optical
crosstalk by construction — the reference the other two have to be measured
against. It is too slow (~390 Hz) and too noisy (~10x mode 2) to play on, and
stays out of deployed configs.

Before it can serve as that reference its own noise floor has to be explained.
The open question is whether the old mode 0 / mode 2 discrepancy was a rail
artefact — mode 0 sagged the shared 3.3 V rail less because it lit one emitter
instead of four — which the per-board buck should now have removed. Full
recipe and acceptance criteria in `03-scan-modes.md`.

Do this when the bench is already set up for benchmarking; nothing else
depends on it.

## Reference: what the scan rate costs us in velocity resolution

`dt` is measured between two threshold crossings of the same sensor, so it is
quantised to one scan period:

| scan rate | quantisation | distinct dt steps in the 8-100 ms window |
|---|---|---|
| 300 Hz (until 2026-09-02) | 3.33 ms | 27 |
| **600 Hz (current)** | **1.67 ms** | **55** |
| 1000 Hz | 1.00 ms | 92 |

Those 27 steps are all the velocity resolution the system can physically
produce. Mapped onto 1-127 that is ~4.7 velocity units per step (visibly
coarse); mapped onto **75-115 it is ~1.5 units per step — finer than anyone
can hear.** So compressing the range does not lose information; it happens to
match the output range to what 300 Hz could resolve; at 600 Hz the same
band carries twice the steps.

Also note anything faster than one scan period (1.67 ms at 600 Hz) is
indistinguishable,
but the curve already saturates at 8 ms, so fast strikes were never resolved
anyway. For a harpsichord none of this matters musically. If higher temporal
resolution is ever wanted for *research* captures, the buck removed the power
ceiling that made higher scan rates expensive — 600 Hz measured at +0.2 W
and is now the production rate.
