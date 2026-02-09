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

## Verification
Manual review is required after replication and before fabrication.

## Disclaimer
No warranty is provided. Users create circuit boards at their own risk.
