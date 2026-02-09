# PHOTON
**PHysical Optical Tracking of Notes system**

PHOTON is a modular, open-source optical sensing platform for high-resolution key and motion tracking. Each module combines a KiCad-designed linear array of **VCNT2025X01** reflective sensors, **TLA2518** high-speed SPI ADCs, and an **RP2350** MCU. Modules can run standalone over USB-C or daisy-chain over RS-485 for large sensing surfaces.

## Highlights
- **Sensors:** VCNT2025X01 reflective array with per-sensor enable lines
- **Digitization:** TLA2518 SPI ADCs for high-speed readout
- **MCU:** RP2350 (dual-core Cortex-M33)
- **Comms:** THVD1424 RS-485 transceivers; main board includes RS-485 bias resistors and controllable termination
- **I/O:** USB-C (power + USB-MIDI/CDC), QWIIC/I2C expansion
- **Open:** KiCad 9 hardware, CircuitPython or C SDK firmware

## Performance
- Single-sensor distance measurement up to ~1 kHz
- Full 64-sensor scan around ~100 Hz

## Architecture (Short)
- Sensor boards: VCNT2025X01 array -> TLA2518 SPI ADCs -> RP2350
- Main board: RP2350 + THVD1424 RS-485 + bias resistors + termination control
- Bus: RS-485 differential, terminated at endpoints

## Getting Started
**Hardware**
- PHOTON module(s)
- USB-C cable
- JST-SH 4-pin cables (1.0 mm pitch, reverse/opposite direction; QWIIC-compatible)

**Software**
- CircuitPython UF2 for RP2350 (custom build recommended)
- KiCad 9 (download: https://www.kicad.org/download/)
- DAW or MIDI viewer (Pianoteq, Ableton Live, Reaper, Max/MSP, etc.)

## Build & Flash
1. Hold **USB-BOOT** and connect via USB-C.
2. Copy the RP2350 CircuitPython `.uf2` to the mounted drive.
3. Copy `code.py` and libraries to the `CIRCUITPY` drive.

## Notes
- Firmware is primarily in CircuitPython; the RS-485 data path uses a C native module for low latency.
- Related repo: https://github.com/w4iei/klavecimbelcircuitpython

## Citation
```bibtex
@article{photon2025,
  title   = {PHOTON: PHysical Optical Tracking of Notes},
  author  = {Anonymous},
  journal = {TBD},
  year    = {2025},
  doi     = {TBD},
}
```
