# PHOTON Embedded Software

CircuitPython firmware for the PHOTON RS-485 harpsichord system.

## Layout
- `src/code.py` is the board entry point. It selects a run mode from marker files on `CIRCUITPY` and falls back to `ACTIVE_MODE` for bench scripts.
- `src/app/rs485_sensor_node/` contains the production sensor-node runtime.
- `src/app/rs485_main_host/` contains the production main-host runtime.
- `src/app/rs485_system_config.py` is the checked-in system configuration source of truth.
- `src/app/sensor_scanner.py` wraps the `photon_sensorscan` C module used by the sensor node scanner.
- `assets/` contains board-local assets such as MIDI files.
- `tools/` contains deployment and reset helpers.
- `../host_code/` contains host-side serial utilities.

## Boot mode selection
`src/code.py` resolves the active role in this order:

1. `/main` on `CIRCUITPY` -> run `rs485-main-host`
2. `/sensor_node_id` (or legacy `/secondary_sensor`) -> run `rs485-sensor-node`
3. otherwise -> run `ACTIVE_MODE` from `src/code.py`

For normal RS-485 deployment you should use the deploy script flags instead of editing `ACTIVE_MODE` directly.

## Configuration
The project no longer uses checked-in JSON pin maps such as `config/pins.json`. Current configuration is split across three places:

1. `src/app/rs485_system_config.py`
   This is the main configuration file in the repo. It defines:
   - host defaults
   - sensor-node defaults
   - per-node overrides in `SENSOR_NODE_OVERRIDES`
   - bus pin assignments under `SENSOR_NODE_CONFIG["pins"]`
   - sensor-node IDs, MIDI range, disabled-sensor defaults, and UART settings

2. `/sensor_node_id` on the device
   This is a plain-text marker file written by `tools/deploy_and_monitor.sh --sensor_node_id N`. The sensor node reads it at boot via `read_sensor_node_id()` and uses it to select the correct `device_id` and any per-node override from `rs485_system_config.py`.

3. `/config/rs485_sensor_node_cal.json` on the device
   This is a runtime-generated calibration file, not a checked-in source file. Sensor nodes load and save calibration there, and the main host reads the same file when checking calibration health or saving calibration data.

In short: edit `src/app/rs485_system_config.py` for committed configuration, and treat `/config/rs485_sensor_node_cal.json` as generated device state.

## Deploying
Use the helper script from this directory:

```bash
tools/deploy_and_monitor.sh
```

Common cases:

```bash
# Deploy as the main host
tools/deploy_and_monitor.sh --main

# Deploy as sensor node 1
tools/deploy_and_monitor.sh --sensor_node_id 1
```

What the script does:
1. Resets the board through USB serial when needed.
2. Copies `src/` and `assets/` to `CIRCUITPY`.
3. Ensures `/sd` and `/config` exist on the device.
4. Installs CircuitPython libraries from `requirements-circuitpy-rs485.txt` by default when `circup` is available.
5. Writes the role marker file (`/main` or `/sensor_node_id`) and reattaches the serial console.

Set `MODE=prod` to copy `src/boot_prod.py` as `boot.py` instead of the default dev boot file.

## Sensor scanner
`src/app/sensor_scanner.py` is still used. The RS-485 sensor node imports it in [src/app/rs485_sensor_node/hardware_setup.py](</Users/noahjaffe/Documents/UvA PhD Work/Harpsichord MIDI Capture/PHOTON_PUBLIC/software/embedded_software/src/app/rs485_sensor_node/hardware_setup.py:10>) and constructs the scanner in `setup_scanner()`. It is the Python wrapper around the `photon_sensorscan` backend and should not be deleted.

## USB serial controls
When USB serial is connected, the production RS-485 flows handle these keys:
- `Enter`: print the current sensor table
- `R`: reboot into calibration mode
- `S`: save calibration and reboot
- `X`: exit calibration without saving and reboot

## Notes
- `src/code.py` still contains many legacy bring-up modes. Some are useful for bench work, but the production path is `rs485-main-host` plus `rs485-sensor-node`.
- Logs use `# LOG` and `# NOTE` prefixes so host tools can filter them reliably.
