## Hardware Overview

KiCad sources for the PHOTON boards. Three designs, all sharing one symbol
library (`PHOTON.kicad_sym`, `PHOTON_Modified.kicad_sym`) and one footprint
library (`photon_common.pretty`) at the top of `hardware/kicad/`.

| Board | Size | Role |
|---|---|---|
| `002_sensor_board` | 413.7 x 35.0 mm | Sensor node — 31 optical sensors |
| `001d_..._low_profile` | 54.0 x 41.7 mm | Bridge, low cost / low profile |
| `001_main_controller_board` | 69.0 x 49.9 mm | Bridge, full size, touchscreen |

## 002 — Sensor node

31 VCNT2025X01 reflective sensors on a **13.3 mm pitch**, read by eight
TLA2518 8-channel ADCs, scanned by an RP2354. One board covers 31 notes, so
**two boards cover a 56-62 key manual** and four cover a double-manual
instrument.

This example is dimensioned for 13.3 mm, measured at either sensing point —
under the **jack** or under the **key lever**. **Every harpsichord is
different.** Measure your own instrument and validate the fit before ordering
boards.

**The board is designed to be shortened.** Mousebite perforations
(`photon_common:mousebite_34mm`) let you snap or cut sections off with
scissors to fit a shorter compass or an awkward case. Signals are routed
*through* the mousebite regions — sensor enables, ADC inputs, VCC and GND —
so a cut section stays functional up to the break.

## 001D — Bridge, low profile

RP2354 (2 MB internal flash), USB-C to the host, THVD1424 to the sensor bus,
microSD. Minimal part count and the smallest board that does the job. This is
the default bridge.

## 001 — Bridge, full size

Same role, plus an **18-pin 0.5 mm FPC connector (FH34SRJ-18S-0.5SH)** for a
touchscreen — the reference panel is a Waveshare 2.8" 240x320, ST7789T3 over
SPI with a CST328 capacitive touch controller over I2C. Carries RP2350 with
external W25Q128 flash and an APS6404L PSRAM, plus one VCNT2025X01 fitted for
bench work. Use this only if the application wants a local display.

## No bridge at all

**Neither bridge board is required.** A sensor node has its own USB-C
receptacle, so running a USB-C cable from one sensor board straight to the
host is a complete system. The bridge exists for installations that want the
host connection somewhere other than at a sensor board, or that want a
touchscreen.

## Power

**The RS-485 cable carries 5 V.** Every board — sensor node and bridge alike —
regulates its own 3.3 V locally with a TPS62A0569ADRLR buck. Nets are just
`+5V` and `+3V3`; there is no shared 3.3 V rail between boards.

Inter-board cable, 4-pin JST-SH:

| Pin | Net |
|---|---|
| 1 | GND |
| 2 | +5V |
| 3 | RS485_P |
| 4 | RS485_N |

Feedback divider is 0.6 V x (1 + 450k/100k) = 3.3 V.

### One host at a time

Every board's USB-C VBUS sits on the same `+5V` net as its RS-485 connectors,
so the whole chain shares one 5 V rail. Powering any single board over USB
powers every board on the bus — which is what makes the bridgeless setup work.

**Do not plug two boards into two different computers at once.** That ties both
hosts' 5 V supplies together through the cable. One USB connection to the
system at a time.

### Budget

A 31-sensor board draws **150-200 mA at 3.3 V** while scanning — mostly the
RP2354 at 150 MHz and eight always-on TLA2518s. Emitters are only ~5% of
system power: one to four are lit at any instant, at ~27 mA each, so peak
current depends on scan mode (~108 mA extra in two-phase, ~216 mA parallel).

COUT is **22 uF**, the value the TPS62A0569A datasheet rates highest with a
1 uH inductor. Do not add distributed bulk on +3V3 to "help" — it parallels
COUT and slows the loop, which costs more than the droop it removes.

### Rail sensitivity

The detector is a 4.7 kOhm pull-up to +3V3 with the ADC referenced to the same
rail, so the dark reading is ratiometric and only the LED's forward-voltage
offset leaks through: **~1.9% of signal per 100 mV of droop**, about 130 counts
on a 6,900-count range against a ~62-count noise floor. Keep emitter-correlated
droop under ~50 mV and it is invisible.

Do not filter AVDD away from the pull-up rail — that breaks the cancellation
and roughly triples the sensitivity.

## Building a sensor board for your instrument

1. Update the schematic so the sensor and bank counts match your instrument.
   The final sensor bank is **not** inherited from the four-sensor bank — edit
   it explicitly.
2. Resize the board outline, copper fills and edge cuts.
3. Run the sensor placement script to set the pitch.
4. Route the first bank and the last bank.
5. Replicate that routing across the middle banks, then clean up and verify by
   hand.

Manual review is required after replication and before fabrication.

## Required KiCad plugins

Both via the Plugin and Content Manager:

- **Replicate Layout** — step 5 above
- **Fabrication Toolkit** — generates the `production/` outputs

## Design recommendations

For a board over 250 mm long, use at least **three M2.5 mounting holes** — one at each
end and one near the middle. The centre screw stops the board bowing away from
the mounting surface. Fit them in a line, not both ends first. Putty is fine
for a temporary setup; screws keep the boards from shifting when the
instrument is moved.

Mounting holes use `photon_common:MountingHole_2.7mm_M2.5_Keepout`, which
carries a 7 mm copper keepout so a screw head cannot bridge the top pour to
the bottom pour.

## Disclaimer

No warranty is provided. Users create circuit boards at their own risk.
