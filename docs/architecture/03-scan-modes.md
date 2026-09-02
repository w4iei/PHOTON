# Scan Modes — Measured Behaviour and Known Defects

Bench findings from the 2026-08-13 power-savings rollout, trimmed to what is
still true. The implementation plan that produced these is complete and has
been removed; so has everything about the old shared-rail power system, which
no longer exists — every board now regulates its own 3.3 V locally from a 5 V
bus (see `hardware/README.md`).

## Two firmware fixes, 2026-09-02

**TLA2518 GPIO0 emitter drive.** On every TLA2518 fitted, GPIO0 configured
as a push-pull output drives high for GPO_VALUE bit 0 = 0 as well as 1;
GPIO2/4/6 obey the datasheet. Slot 3 of every bank is switched through
GPIO0, so those seven emitters per board had been on continuously since
bring-up: hot and dim (half the signal at full key travel, a late strike
point and a compressed velocity window on every fourth key), and lighting
their neighbours during every read. `tla2518_emitters()` now switches GPIO0
through GPO_DRIVE_CFG (push-pull = on, open-drain = off, both properly
driven); init parks it open-drain. Verified per sensor with the emitter
commanded on and off: slot-3 sensors now swing ~2,700 -> ~65,000 like the
rest, and B4 on node 4 reads ~30,000 at full travel instead of 12,000.
Every board needs recalibrating after this fix (slot-3 maxima roughly
double, rest levels drop a few hundred counts).

**SPI bus speed: 10 MHz (validated).** With slot-3 emitters actually
switching, node 1 lost ~3% of register writes to the ADC nearest the MCU
(bank 3, 22 mm) in the mode-2 frame sequence — one-sweep dark reads on its
slot-3 sensor, ~18 spurious notes per second at rest. Near-end reflection
on an under-terminated line; a hardware matter (series terminations, stub
lengths — the latest board revision shortens the inner ICs' stubs). At
10 MHz: zero such reads in 12,000 sweeps, zero events at rest. Sweep times:
mode 2 969 us, mode 1 766 us, mode 0 2,475 us; mode 2 idles ~40% at 600 Hz.
ADC conversion and oversampling are internal and unaffected.

The August mode-0 findings below (~10x noisier, modes not converging,
idx 28) were measured with seven emitters per board permanently on and
must be re-measured before any of it is believed.

## Production configuration

**Mode 2 (two-phase), 600 Hz, 50 us settle.** (Compiled default since
2026-09-02; was 300 Hz. 600 Hz halves the dt quantisation to 1.67 ms — 55
velocity steps across the 8-100 ms window instead of 27 — and costs 0.2 W,
see the rev 1D measurements below.)

Two-phase hits the peak-current goal that originally motivated sequential
(~108 mA vs parallel's ~216 mA) while keeping 8-key-pitch spacing between
simultaneously lit emitters, against parallel's 4. Verified uniform ~3.6-8.4k
rest across all 31 sensors including idx 28, with ~30 counts of noise.

## Measured scan cost per mode

Board 4, measured at the ADC:

| Mode | Sweep | Peak emitter current | Max usable rate |
|---|---|---|---|
| 0 sequential | 2,536 us | ~27 mA | ~390 Hz |
| 2 two-phase | 826 us | ~108 mA | ~1.2 kHz |
| 1 parallel | 585 us | ~216 mA | ~1.7 kHz |

Sequential is lowest on both peak and average current: in parallel mode every
emitter stays lit through the other sensors' ADC reads, so most of that
emitter-on time is wasted. It also eliminates optical crosstalk by
construction — which is what makes it a useful measurement reference rather
than a performance mode. See below.

## Scan rate and mode are not power levers

Two clean measurements at verified-uniform fleet states overturned the premise
the original plan was built on:

| Fleet state | Emitter duty | Measured system power |
|---|---|---|
| 200 Hz sequential | 51% | 3.93 W |
| 300 Hz sequential | 76% | 4.00 W |

Solving those: **fixed baseline ~3.79 W (95%), emitters ~5%.** A 1.5x change in
emitter duty moves total power by 0.07 W. The baseline is five RP2350-class
MCUs at 150 MHz, forty permanently-powered TLA2518 ADCs and five RS-485
transceivers.

So set the scan rate for the temporal resolution you want, not to save power.
Mode choice is a **peak current** and crosstalk decision, not an energy one.

### Rev 1D (per-board buck) re-measurement, 2026-09-02

Whole system (bridge + four nodes + 1 m cable) at a USB power meter on the
host side, 5.12 V throughout. Meter resolution 10 mA (~50 mW).

| mode | rate | A | W | vs baseline |
|---|---|---|---|---|
| 2 two-phase | 300 Hz | 0.59 | 3.02 | baseline |
| 2 two-phase | **600 Hz** | 0.63 | 3.23 | +0.20 |
| 1 parallel | 300 Hz | 0.59 | 3.02 | 0 |
| 1 parallel | 400 Hz | 0.61 | 3.12 | +0.10 |
| 0 sequential | 300 Hz | 0.59 | 3.02 | 0 |

Same conclusion, now on the buck: the floor is ~3.0 W and mode does not move
it at all. That is arithmetic, not a stuck setting (the modes were verified
switching by their distinct rest readings): every emitter is lit for about
one settle-plus-read window per sweep in every mode, so mode sets the *peak*
current (27 / 108 / 216 mA) and leaves the per-sweep energy nearly constant.
Rate is the only scan lever, ~0.1 W per 100 Hz, and doubling it (which also
doubles ADC/SPI work) cost 40 mA — an upper bound on the whole emitter
budget at 300 Hz. The LDO-era 3.93 W implies the same ~2.6 W 3.3 V load
through a fixed 5:3.3 ratio; through an ~88% buck that predicts 2.95 W,
matching the 3.02 W measured. Nothing on the boards got cheaper; the LDO
stopped burning a third of the input as heat.

RS-485 on the rev 1D transceiver, same session: 20 s at 100 ev/s/node,
1,000 polls/s per node, zero gaps/dup/malformed, crc_err=0 hdr_err=0.

Remaining levers, none scan-related: MCU clock (careful — `clk_peri` is pinned
for the 4 Mbaud UART divisor), core 1 idle (it busy-spins ~24% of the period at
300 Hz; a WFE wait would recover some), and TLA2518 standby modes.

## Mode 0 (sequential): a benchmarking instrument, not a performance mode

Mode 0 lights one emitter at a time, so it is the only mode with **zero
optical crosstalk by construction**. That is what it is for: parallel and
two-phase can only be checked for crosstalk by comparing them against
something that has none.

It is not a candidate for playing. The 2,536 us sweep caps it at ~390 Hz and
it reads ~10x noisier than mode 2. Keep it selectable — from the console and
via `scan_mode` in the config store — and keep it out of deployed
configurations.

### History

Sequential read slots 1-3 elevated and collapsed sensor idx 28 to a pinned,
noisy ~31k with no key response on every board — which calibrates into a
sliver of range and machine-guns that note.

**Fix applied (commit after c6f784c):** `sweep_sequential` reordered to write
`CHANNEL_SEL` *under* the settle window, matching the proven `step_banks`
ordering, so the ADC mux gets settle time connected to the new channel.

**Validation 2026-08-13, board 1 — PARTIAL PASS.** The catastrophic failure is
gone (idx 28: pinned 31,064 at 750-1300 noise, to 10,876 at 240-380). But
modes still do not converge: 8 sensors differ >15%, idx 28 by 44%, and mode 0
stays ~10x noisier.

### TODO — characterise mode 0 at the next benchmarking pass

Mode 0 cannot serve as the crosstalk reference until its own noise floor is
understood, so this blocks any crosstalk measurement of modes 1 and 2.

The leading hypothesis for the residual was that mode 0 lights one emitter
where mode 2 lights four, so the 3.3 V rail sagged less and mode 0's higher
readings were partly *real* — a rail artefact, not a scan defect. That rail
was shared across four boards through the cable. It is now regulated locally
on every board, so if the hypothesis was right the discrepancy should have
shrunk or vanished.

Recipe: flash one node, A/B `mode 0` against `mode 2` via `data <id>`, at rest
and under a held key. Judge on **noise and repeatability**, not on agreement
with mode 2 — if the old discrepancy was a rail artefact then "the modes must
converge" was always the wrong acceptance test. The 10x noise gap needs
explaining either way.

## Diagnostic worth keeping

**All 31 channels reading saturated (~65,200-65,520) is a power symptom, not an
optical or firmware one.** It means the analog rail has drooped. Check with
`data <id>` from the bridge. The original cause — one regulator sourcing four
boards through the cable — is designed out, but the signature still identifies
a rail problem faster than anything else.

## Open item

Disabling a sensor on a node (the local mask that also stops its emitter) is
still USB-only; the bridge's `disable` targets the global note map instead. Add
NODECTL op 5 (set local disabled mask) if a board ever needs masking after the
instrument is closed up.
