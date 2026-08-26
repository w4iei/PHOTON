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

**Bulk capacitance: sized by the regulator, not by habit.** An earlier version
of this note called for ~1 mF per board. That figure assumed the old topology,
where a regulator at the far end of a cable could not respond within a sweep.
It does not apply once each board regulates locally:

- Only **one emitter per bank is lit at a time** in every scan mode, so the step
  at each TLA2518 cluster is ~30 mA, not the board aggregate. The existing
  3 x 1 uF per cluster covers that with the copper pour behind it — well under
  a millivolt.
- The 108-216 mA figure is the *aggregate* across the board. That is a control
  loop problem, and it is answered by the buck's output capacitor.
- TPS62A0569A datasheet Table 8-3 rates **22 uF as `++`** for 1.8 V <= VOUT with
  a 1 uH inductor, and marks it the EVM configuration. 2 x 22 uF and 4 x 22 uF
  drop to `+`: less droop per transient, but reduced gain bandwidth and a longer
  return to target — which matters here, because the ADC samples slot by slot
  across the sweep, so a slow recovery smears the error across more slots
  instead of just the first.

Distributed bulk on the 3.3 V rail sits in parallel with COUT and pushes the
loop toward the `+` cells, so **adding it is a downgrade, not insurance.** Leave
COUT at 22 uF and add nothing at the clusters. Bias derating is already included
in the datasheet's recommendation, but only for the specified part family — a
6.3 V part in place of the 10 V one is not covered.

Rail sensitivity, for sizing any future argument about droop: the detector is a
4.7 kOhm pull-up to +3V3 with the ADC referenced to the same rail, so the dark
reading is exactly ratiometric and only the LED's forward-voltage offset leaks
through. That works out to **~1.9% of signal per 100 mV of rail droop**, or
~130 counts on a 6,900-count range against a ~62-count noise floor. Keep
emitter-correlated droop under ~50 mV and it is invisible. Do not "improve" this
by filtering AVDD away from the pull-up rail — that breaks the cancellation and
roughly triples the sensitivity.

## Bus voltage: 5 V distribution (plan of record)

Inter-board cables carry **5 V**, and every board regulates its own 3.3 V
locally with a TPS62A0569ADRLR. This supersedes the "either/or" framing above:
it is the design of record for all new boards.

Because the boards are revised in lock-step, this is a clean break rather than a
migration: all new boards go to 5 V together and the 3.3 V-era boards are
retired as a set. There is no supported mixed-voltage configuration.

### Why

- **The rail that matters is regulated at the point of use.** Under 3.3 V
  distribution a sensor board's buck sits in dropout with no active loop at
  all, so the rail droops with the emitters through the high-side FET, the
  inductor and the entire cable run — and the droop scales with the board's
  position in the chain. On 5 V the loop holds it, identically on board 1 and
  board 4.
- Each board's emitter transient is sourced by its own output capacitor instead
  of being pulled through three cables and three boards' copper.
- No shared analog rail, so the one-regulator-for-four-boards failure class
  documented above cannot recur.
- Cable current falls for the same delivered power (~600 mA at the worst node
  instead of ~750 mA), so I^2R loss in the cables drops by ~56%.

### Topology

```
   USB-C power pin              SS14              JST power pins (J1.2/J2.2)
  ──────────────────────  A ──▶|── K  ──────────────────────────┬─────────────
           │                                        │           │
          CR1                                      CR2         FB1
           │                                        │           │
          GND                                      GND      U5.VIN -- buck -- +3V3
```

`J1.2` and `J2.2` are joined by direct copper, so the chain passes through the
board without going near the buck.

**These are three separate nets**, and the two 5 V ones must NOT share a name or
the diode is shorted out by its own netlist. Avoid calling the JST rail "VBUS" —
that is the USB-C spec's name for the *connector* pin, and reusing it for the
chain rail misleads every later reader:

| Net | Members |
|---|---|
| `USB_VBUS` — USB connector only, diode anode side | J3 A4/A9/B4/B9 · **SS14 pad 2 (A)** · **CR1** · 100k bleed to GND |
| `CHAIN_5V` — the inter-board bus, diode cathode side | **SS14 pad 1 (K)** · J1.2 · J2.2 · FB1.1 · **CR2** |
| `VIN_LOCAL` — buck input, after the bead | FB1.2 · U6.3 (VIN) · U6.5 (EN) · C1 4.7 uF |

**One clamp per side of the diode.** A strike on a JST power pin cannot reach a
clamp on the USB side — it would have to run backwards through the SS14, which
is precisely what the diode prevents. A single TVS therefore leaves whichever
side it is not on completely bare.

Execution needs no extra parts: **CR1 stays where it already is** (the USB-C
power pin) and **CR2 relocates** from `+3V3` to `CHAIN_5V`, since `+3V3` is now
a local net behind the buck that reaches no connector.

C1 belongs on the buck side of FB1, at the pin — the datasheet requires the
input capacitor "as close as possible between VIN and GND", so it does not sit
on `+5V`.

**Use a TVS on each of the two nets, not one shared part.** CR1 moves to `+5V`
(the net reaching the JST connectors and running the length of the instrument),
and a second goes on `VBUS`. Splitting the nets otherwise leaves the SS14's
anode unclamped, which is a regression from the single-net board. Schottkys are
more susceptible to reverse-bias ESD than PN rectifiers — the metal-semiconductor
junction is thinner and more localised — and a negative strike on the USB pin
reverse-biases the SS14 against only its 40 V rating. Forward strikes are a
non-issue: they conduct through to the `+5V` clamp.

Note the failure mode: an ESD-damaged Schottky fails **shorted**, silently
re-merging the nets and restoring the live receptacle with no other symptom.
The 100k bleed makes that observable — bus powered with USB unplugged, `VBUS`
should read ~0 V; a solid 5 V means the diode is backwards or failed short.

- The bus **passes through on direct copper** between J1.2 and J2.2. Do not
  route the chain through the buck's VIN node.
- +5V feeds a 2.5-5.5 V buck input, so it does **not** need a pour. A 1 mm
  1 oz trace over 407 mm is ~204 mOhm — ~122 mV at the 600 mA worst node, which
  is irrelevant to a regulator with a 3 V input window.
- +3V3 becomes a local net and stops carrying pass-through current for
  downstream boards: ~200 mA instead of ~750 mA in the copper that matters.
- Tap VIN from the **connector side of FB1** so only the local board's ~150 mA
  passes through the bead, not the chain's. (FB1 = Murata BLM18PG121SN1D,
  LCSC C14709: 120 Ohm @ 100 MHz, **2 A**, 50 mOhm DCR. On main rev 1D this bead
  is in series with the entire system input current — 0.54 A measured, 27% of
  rating. Fine, but it is a single point worth remembering.)
- **SS14 (LCSC C2480, JLCPCB basic part) sits at J3.VBUS, ahead of FB1**, anode
  on the connector. Orientation, since `Device:D_Schottky` numbers the cathode
  first and it is easy to get backwards: **pad 2 (A) -> J3.VBUS, pad 1 (K) ->
  FB1 / J1.2 / J2.2**. On the SMA footprint pad 1 is the banded end, so the
  stripe faces into the board. Check after assembly: bus-powered with USB
  unplugged, the USB VBUS pin should read ~0 V, and the board should still come
  up from its own USB with the bus disconnected. It blocks the *bus from back-feeding the USB connector*, so
  an unmated receptacle is not left live and two sources (a charger on the
  bridge, a laptop on a sensor board) cannot push into each other. Because it is
  in the USB leg and not the bus leg, **the bus path carries no diode drop** — 
  the 0.3 V is paid only when running from that board's own USB, and the board
  stays tolerant of either bus voltage.
- Note what this orientation does *not* do: USB can still energise the bus. That
  is harmless in a uniform 5 V fleet, but it is fatal to a 3.3 V-era board, which
  is why fleet separation is mechanical (see below) rather than electrical.
- SS14 reverse leakage is not negligible (hundreds of uA at full reverse rating,
  far less at 5 V, worse hot), so the unmated VBUS pin will float up through it.
  Add a **100 kOhm bleed to GND on J3.VBUS** if you want that pin to actually
  read 0 V.
- **Do not adjust the feedback divider to compensate for the SS14.** The diode
  is on the *input*; the divider sets the *output*. At 5 V in, VIN = 4.7 V while
  the buck needs only ~3.33 V to regulate, so it delivers 3.3 V regardless — 
  there is nothing to compensate. At 3.3 V in it is at 100% duty with the loop
  already railed, so raising the target changes nothing. Divider stays at
  0.6 x (1 + 450k/100k) = 3.3 V.

### Back-powering: resolved by construction

Under 3.3 V distribution the cable drove the buck's *output* while its input was
unpowered. That is outside two datasheet limits at once — Absolute Maximum
`SW: -0.3 V to VIN + 0.3 V`, and Recommended Operating `VOUT: 0.6 V to VIN` — 
and it produces four separate mechanisms:

1. SW driven above VIN, clamped only by the high-side body diode conducting into
   VIN.
2. VIN back-charges to ~2.7 V. Since VIN is the VBUS net, that appears on the
   USB-C connector.
3. VIN crossing UVLO (2.3-2.5 V) enables the part, because EN is strapped to
   VIN and its threshold is 0.9 V. The **A suffix is the FPWM variant**, which
   permits reverse inductor current, so an enabled part actively pumps charge
   from the rail back into VIN.
4. While disabled it runs **active output discharge, 68 mA typ on SW** — ~270 mA
   of dead load across four boards.

With 5 V distribution the 3.3 V rail is sourced only by the buck output and is
never driven externally, so none of these paths exist. No jumper, no mode
switch, nothing to remember.

### Dropout: 3.3 V in still works

Fed 3.3 V, the part enters 100% duty (datasheet 7.3.2) and passes it through:
`VIN(MIN) = VOUT + IOUT x (RDS(ON) + RL)` ~= 3.3 + 0.2 x 0.13 = **3.33 V**, so
the output lands ~26 mV low. Ratiometric, below the noise floor, no
recalibration. With the SS14 in the USB leg rather than the bus leg, this holds
whether or not the diode is fitted. (Had it been placed in the bus path, VIN
would drop to 3.0 V and the output to ~2.97 V — below the THVD1424's 3.0 V
minimum VCC. That is the reason for the placement.)

### Mixed fleet: the 3.3 V-era boards

Six existing boards carry **+3V3 on pin 2 of a 4-pin JST-SH** with an identical
pinout: main rev 1D (J102/J103/J104) and the four sensor boards (J1/J2). A 5 V
cable into any of them puts 5 V straight onto the rail feeding the RP2354 and
the TLA2518s. Nothing physical prevents the mis-mate.

There is no cheap electrical fix: a TVS standing off 3.3 V clamps around 6-9 V,
well above the RP2354's ~4 V absolute maximum, and this is a sustained DC
condition rather than a transient, so damping does not help either.

**Therefore: 5 V boards get a physically different connector** — a 5-position
JST-SH, or a different family such as PicoBlade. Mechanical keying is the only
mitigation that survives a tired person in a workshop. The 4-pin JST-SH becomes
the permanent marker for "3.3 V-era board".

Note the hazard is one-directional. 1D sourcing 3.3 V into a *new* sensor board
is safe on the bus voltage alone (the buck drops into pass-through). What kills
1D is plugging **USB into a new sensor board** while it is cabled to 1D, because
USB feeds the bus through the SS14 — and that is exactly the NODECTL-bootsel
flashing sequence. The SS14 does **not** prevent this: it blocks the bus from
back-feeding USB, not the reverse. The only mitigation is the keyed connector,
plus retiring the 3.3 V boards as a set.

### ESD

HBM (+/-2 kV) and CDM (+/-500 V) on the TPS62A0569A are *handling* ratings — the
datasheet's own JEDEC footnotes scope them to "safe manufacturing with a standard
ESD control process". They are not system-level protection for a touchable
connector; that is IEC 61000-4-2, whose pulse (150 pF/330 Ohm) is far more
energetic than HBM's (100 pF/1500 Ohm). Use **one TVS per net** — CR2 on `CHAIN_5V`, CR1 staying on `USB_VBUS` — and verify the
standoff is rated for 5 V. Both existing parts are the same part number on a
5 V rail and a 3.3 V rail, which cannot be correct for both.

No external TVS is needed on RS485_P/RS485_N: the THVD1424 carries
**+/-8 kV IEC 61000-4-2 contact, +/-15 kV air and +/-4 kV EFT** on its bus pins.
The `+5V` clamp guards the JST connectors' *power* pin only; the data pair is
covered by the transceiver.

Part count is unchanged: CR1 stays on the USB pin and CR2 relocates from `+3V3`
to `CHAIN_5V`. (The only +3V3 still leaving the board is J4, the SWD header — an
internal debug connector, so a clamp there is optional.)

Keep hot-plug separate from ESD. A 5 V TVS clamps around 9-10 V, above the
6.5 V VIN absolute maximum it would be protecting, so clamping cannot help
there — hot-plug overshoot is a damping problem. It is currently unquantified;
scope a live cable insertion before adding any part for it.

## TODO — next revisions
- **Rev-bump the second main controller board** to the rev 1D power system:
  buck regulator instead of the NCP1117, and RP2354 (2 MB internal flash) in
  place of RP2350 + external W25Q128. See the LDO-vs-buck measurements above.
  Fold the 5 V bus and the keyed connector into the same revision.
- **Deprecate main controller rev 1D.** Its bus connectors carry +3V3 directly,
  so it cannot safely share a bus with 5 V boards. Until it is respun, do not
  run new sensor boards from a 1D board.
- **Key the 5 V connectors** so they cannot mate with a 3.3 V-era board.
- Sensor board next revision: J1/J2 pin 2 -> **`CHAIN_5V`** (a distinct net
  from `USB_VBUS`, joined only through the SS14) with direct J1-J2 pass-through, FB1
  between `+5V` and the buck input, SS14 at J3.VBUS, +3V3 local to the buck,
  JP1 retired.
- Add a **CFF footprint across the buck feedback divider, 22-47 pF**,
  populate-on-test. The datasheet gives 10-120 pF for R2 = 100 kOhm, which is
  exactly R103. With a 450 kOhm top leg the feedback node is ~82 kOhm Thevenin
  and the loop is sluggish without it.
- **Two TVS, one per net**: CR2 on `CHAIN_5V`, CR1 staying on `USB_VBUS`. Drop CR2 (+3V3 is
  local now). Verify the standoff is a 5 V part — the current part number is used
  on both a 5 V and a 3.3 V rail and cannot suit both.
- Verify **C4** is the 10 V X7R 0805, not 6.3 V.
- Add a **100 kOhm bleed from `USB_VBUS` to GND** so an unmated receptacle reads 0 V
  and a shorted SS14 is detectable.
- Bench measurement that validates the lot at once: scope the 3.3 V rail
  AC-coupled at the far end of the board, 20 mV/div, triggered on a sweep.
  Under ~50 mV of emitter-correlated droop and there is nothing left to do.

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
