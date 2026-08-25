# Next Bench Session — Plan

Short and ordered.

**Step 2a is software-only and must happen before any config field is added** —
it is the one item that can silently destroy all four boards' calibration.
Everything else needs the hardware connected.

## 1. Power characterisation + RS-485 sanity (~15 min)

Now that main rev 1D uses a buck, re-measure with a USB power meter at the
5 V input. Set each config from the bridge console (`mode <m> <id>`,
`rate <hz> <id>`, all four nodes), let it settle ~10 s, read the meter:

| mode | rate | note |
|---|---|---|
| 2 two-phase | 300 Hz | production config — the baseline |
| 2 two-phase | 600 Hz | does rate matter now the LDO loss is gone? |
| 1 parallel | 300 Hz | 2x peak emitter current |
| 1 parallel | 400 Hz | old `full_performance` operating point |

Expected: still dominated by fixed baseline (~2.4 W delivered), with mode/rate
worth only a few hundred mW. Record in `hardware/README.md` next to the
LDO-vs-buck table. **Do NOT test mode 0** — quarantined, see 03-plan.

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
sample library without reflashing. Bridge-side only — velocity is computed in
`midi_map_velocity()` on the bridge.

### 2a. FIRST: generalise the config-layout migration (do this before 2b)

Pure software, no hardware needed, testable on the host. **Do it before
appending any field**, or all four boards lose calibration and node ids the
moment they are flashed.

Why. The config record ends with a CRC32 over everything preceding it, so the
reader must know the record's exact length to locate and verify it:

| | layout | ends with |
|---|---|---|
| v1 | original | `... vel_curve, crc` |
| **v2** | **what all four boards hold today** | `... vel_curve, manual_channel[3], crc` |
| v3 | after appending velocity range | `... manual_channel[3], vel_out_min, vel_out_max, crc` |

`sector_valid()` currently tries exactly TWO offsets: the current struct size,
and "current size minus `sizeof(manual_channel)`". Today those are v2 and v1,
so both are covered. Make v3 current and those two become v3 and *v3 minus 3
bytes* — which is **not v2**. Every board's record then fails both checks,
`config_store_init()` calls `load_defaults()`, calibration is wiped and every
node returns as `node_id = 1` (all four answering at the same bus address).

Note reflashing is NOT the hazard — config lives above the program region and
survives. The hazard is the new firmware failing to *recognise* what survived.

Fix (~10 lines in `sector_valid()`): replace the single hardcoded fallback
with a loop over a table of known historical record sizes. The struct only
ever grows by appending, so size uniquely identifies the layout:

```c
// Historical record sizes, newest first. Append one entry whenever a field
// is added to photon_config_t; never reorder or remove entries.
static const size_t k_layout_sizes[] = {
    sizeof(photon_config_t),                                    // v3
    offsetof(photon_config_t, vel_out_min) + sizeof(uint32_t),  // v2 + crc
    offsetof(photon_config_t, manual_channel) + sizeof(uint32_t)// v1 + crc
};
```
For each size: read the CRC from `base + size - 4`, verify over the preceding
`size - 4` bytes, and on a match zero-fill the remainder of the struct so new
fields take their defaults. Add a host test in `firmware/test/host` that
builds a v1 and a v2 record and asserts both load with node id and cal_min
intact.

### 2b. Then: the velocity change itself

## 3. Rev-bump the second controller board

Bring board 001 (the other main controller) to the rev 1D power system:
buck instead of NCP1117, and RP2354 instead of RP2350 + external W25Q128.
See the LDO-vs-buck measurements and the RP2354 migration notes in
`hardware/README.md`. Firmware needs no change — one UF2 already covers both
flash sizes.

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
