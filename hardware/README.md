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
