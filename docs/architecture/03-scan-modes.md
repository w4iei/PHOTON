# Scan Modes — Measured Behaviour and Known Defects

Bench findings from the 2026-08-13 power-savings rollout, trimmed to what is
still true. The implementation plan that produced these is complete and has
been removed; so has everything about the old shared-rail power system, which
no longer exists — every board now regulates its own 3.3 V locally from a 5 V
bus (see `hardware/README.md`).

## Production configuration

**Mode 2 (two-phase), 300 Hz, 50 us settle.**

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
construction. Those are the reasons to want it — see the defect below for why
it is not available.

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

Remaining levers, none scan-related: MCU clock (careful — `clk_peri` is pinned
for the 4 Mbaud UART divisor), core 1 idle (it busy-spins ~24% of the period at
300 Hz; a WFE wait would recover some), and TLA2518 standby modes.

## DEFECT: mode 0 (sequential) is quarantined — do not deploy

Mode 0 remains selectable from the console and via `scan_mode` in the config
store. It must not be used.

Sequential reads slots 1-3 elevated and, before the fix below, collapsed sensor
idx 28 to a pinned, noisy ~31k with no key response on every board — which
calibrates into a sliver of range and machine-guns that note.

**Fix applied (commit after c6f784c):** `sweep_sequential` reordered to write
`CHANNEL_SEL` *under* the settle window, matching the proven `step_banks`
ordering, so the ADC mux gets settle time connected to the new channel.

**Validation 2026-08-13, board 1 — PARTIAL PASS.** The catastrophic failure is
gone (idx 28: pinned 31,064 at 750-1300 noise, to 10,876 at 240-380). But modes
still do not converge: 8 sensors differ >15%, idx 28 by 44%, and mode 0 stays
~10x noisier.

**Worth re-testing now.** The leading hypothesis for the residual was that
mode 0 lights one emitter where mode 2 lights four, so the 3.3 V rail sagged
less and mode 0's higher readings were partly *real* — a rail artefact, not a
scan defect. That rail was shared across four boards through the cable. It is
now regulated locally on every board, so if the hypothesis was right the
discrepancy should have shrunk or vanished.

Retest recipe: flash one node, A/B `mode 0` against `mode 2` via `data <id>`,
at rest and under a held key. Judge on **noise and repeatability**, not on
agreement with mode 2 — if the old discrepancy was a rail artefact then
"the modes must converge" was always the wrong acceptance test. The 10x noise
gap needs explaining either way.

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
