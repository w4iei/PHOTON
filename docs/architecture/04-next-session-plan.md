# Next Bench Session — Plan

Short and ordered.

Step 2 is software-only and testable on the host; it deliberately resets every
board's stored config once (see 2a), so budget a `setid` + recalibration pass
with it. Everything else needs the hardware connected.

## 1. Power characterisation + RS-485 sanity (~15 min)

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

### 2a. Config layout: take the one-time reset, do not carry a migration table

Appending two floats to `photon_config_t` lengthens the record, and the CRC32
sits at the end — so the reader must know the exact length to locate it.
`sector_valid_at()` tries two lengths: the current `sizeof`, and current minus
`sizeof(manual_channel)`. After appending, those become v3 and *v3 minus 3
bytes*; the v2 layout every board currently holds matches neither. Every
record fails and `config_store_init()` calls `load_defaults()`.

**That is the accepted outcome.** The alternative — a table of historical
record sizes, a host test for it, and a "never reorder these entries" comment
carried forever — is permanent complexity bought to avoid one afternoon of
reconfiguration. Decided against.

Two things follow that are easy to get wrong:

- **Erasing the config sectors first is pointless.** A stale record fails CRC
  and loads defaults; an erased sector loads defaults. Identical end state.
  There is no wipe procedure — flash, then reconfigure.
- **Reflashing was never the hazard.** Config lives above the program region
  and survives a UF2 drop. The hazard is only that the new firmware stops
  *recognising* what survived.

So `sector_valid_at()` should **lose** its migration fallback in this change
and check the current length only. Net effect on `config_store.c` is fewer
lines than it has today.

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

### 2c. Then: the velocity change itself

- `vel_out_min` / `vel_out_max` appended to `photon_config_t`, defaults 75/115
- `midi_map_velocity()` uses the compressed range
- `velrange <min> <max>` console command, local + NODECTL, persisted
- `sector_valid_at()` migration fallback removed

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
| **300 Hz (current)** | **3.33 ms** | **27** |
| 600 Hz | 1.67 ms | 55 |
| 1000 Hz | 1.00 ms | 92 |

Those 27 steps are all the velocity resolution the system can physically
produce. Mapped onto 1-127 that is ~4.7 velocity units per step (visibly
coarse); mapped onto **75-115 it is ~1.5 units per step — finer than anyone
can hear.** So compressing the range does not lose information; it happens to
match the output range to what 300 Hz can actually resolve.

Also note anything faster than one scan period (3.33 ms) is indistinguishable,
but the curve already saturates at 8 ms, so fast strikes were never resolved
anyway. For a harpsichord none of this matters musically. If higher temporal
resolution is ever wanted for *research* captures, the buck removed the power
ceiling that made higher scan rates expensive — 600 Hz is now affordable.
