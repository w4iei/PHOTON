## Hardware Overview

This directory contains the KiCad sources for the PHOTON main controller board and the sensor node boards.

## Boards
- Main controller board: usable as-is for most builds.
- Sensor node board: must be adapted to the target instrument’s geometry and mounting constraints.

## Sensor Node Customization Flow
1. Update the schematic so the number of sensors and sensor banks matches your instrument. The final sensor bank is not inherited from the four-sensor bank, so update it explicitly.
2. Resize the overall board outline, copper fills, and edge cuts to fit the instrument.
3. Run the sensor placement script to place sensors at the correct pitch.
4. Route the first bank and the last bank.
5. Replicate the routing/layout for the middle banks, then perform manual cleanup and verification.

## Required KiCad Plugins
- Install the KiCad Replicate Layout plugin via the KiCad Plugin and Content Manager.
- Install the KiCad Fabrication Toolkit the same way.

## Design Recommendations
- For a PCB greater than 250mm long, it is recommended to have three mounting holes for M2.5 (wood) screws to secure the boards in place. Putty (e.g. Blu Tack) is fine for a temporary setup, but screws ensure stability over the long term, including preventing the PCBs from shifting during moving. Having one mounting hole on each end and one in the center (doesn't need to be exactly in the center) ensures that the PCB doesn't bow or flex in the middle and raise up above the wood mounting surface. Install the screws in a linear fashion — not both ends and then the center.

## Power Budget
A 31-sensor board draws roughly **150-200 mA** at 3.3 V while scanning: mostly
the RP2350 at 150 MHz plus eight always-on TLA2518 ADCs, with the emitters a
surprisingly small share (~5% of system power — only one to four are lit at any
instant depending on scan mode). Peak current is set by the scan mode: ~27 mA
of emitter per lit LED, so ~108 mA extra in two-phase (4 lit) and ~216 mA in
parallel (8 lit).

**The RP2350 reference-design LDO (NCP1117, 1.0 A, SOT-223) realistically
powers about two boards.** Note that is a *linear* regulator: it burns
(Vin - 3.3 V) x I as heat, so at 5 V in it dissipates ~1.7 W per amp in a
package that sheds roughly 1 W. Four boards fed from a single board's LDO
exceeded both its current rating and its thermal limit — the symptom is not a
clean shutdown but **sensor ADCs reading saturated (~65,000 on every channel)**
as the analog rail droops, which looks deceptively like an optical or firmware
fault.

For more than two boards, either:
- distribute **5 V** on the inter-board cables so each board regulates locally
  with its own LDO (the part is already fitted on every sensor board), or
- feed the shared 3.3 V rail from a **buck converter** sized for the whole
  chain, rather than from one board's linear regulator.

### Measured: LDO vs buck on the four-board chain

Main controller board rev 1D replaced the NCP1117 with a buck. Same four
sensor boards, same firmware, same scan settings, measured at the 5 V input:

| | NCP1117 (linear) | Buck |
|---|---|---|
| Input power | 3.93 W (0.79 A @ 5 V) | **2.70 W (0.54 A @ 5 V)** |
| Conversion efficiency | 3.3/5 = **66%**, fixed | ~90% |
| Delivered to the 3.3 V rail | ~2.6 W | ~2.4 W |
| Wasted as heat | **~1.34 W** | ~0.27 W |
| Board temperature | hot to the touch | cool |

**The load did not change** — the 3.3 V-side current is ~0.75 A either way.
The 1.2 W saved is almost exactly the heat the LDO was dissipating across its
1.7 V drop.

How far outside its ratings the linear part was running:

| Scan setting | LDO current | vs 1.0 A rating | Heat | vs ~1 W SOT-223 capability |
|---|---|---|---|---|
| 400 Hz parallel | 1.00 A | **at the limit** | 1.70 W | **170%** |
| 300 Hz sequential | 0.79 A | 79% | 1.34 W | **134%** |

Note the thermal limit binds well before the current limit. **Firmware tuning
could not have fixed this**: even after scan-mode and rate optimisation cut
system power from 5 W to 3.93 W, the regulator was still at 134% of its
thermal budget, because the emitters are only ~5% of system power while the
regulator's 66% efficiency accounted for ~34% of it. Use a buck for any chain
beyond two boards.

**Add more bulk capacitance on the next revision.** The emitter load is
pulsed, not steady: a sweep lights LEDs for a few hundred microseconds at a
time, so the board asks for its peak current in bursts that the cable and
regulator have to service. Local bulk capacitance supplies those bursts so the
upstream supply only has to deliver the average. Size it for the pulse rather
than by habit — holding ~150 mA for a ~600 us sweep within 100 mV needs on the
order of **1 mF per board** (e.g. 2 x 470 uF polymer/electrolytic near the
emitter drivers), not the 10-47 uF normally sprinkled around. Caps only fix the
*transient* component: they do nothing for steady-state droop caused by cable
resistance or an undersized regulator, so they complement the 5 V distribution
change above rather than replacing it.

## Verification
Manual review is required after replication and before fabrication.

## Fully Complete Example
### 001 Main Controller Board
Exists as a main controler board that should be able to be used without modification. 
It includes extra items that can be removed, if desired. For example, the VCNT2025X01 sensor is populated for debugging and proof of concepts. 
It is not necessary for use as a main controller board. 

### 002_sensor_board_example
This project exists as a reference design for the sensor board. 


## Disclaimer
No warranty is provided. Users create circuit boards at their own risk.
