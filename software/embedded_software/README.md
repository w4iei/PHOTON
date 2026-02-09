# Harpsichord MIDI Dev Board – Software

CircuitPython firmware and host-side tooling for the harpsichord sensor dev board.

## Layout
- `midi_controller_embedded_sw/` – primary CircuitPython sources.
  - `src/code.py` boots the board and dispatches into the selected run mode.
  - `src/app/` contains the production controller plus the legacy bring-up scripts.
  - `assets/` holds MIDI files that can be played back directly from the board.
  - `config/` captures board-specific JSON overrides (masking, calibration, etc.).
- `host_code/` – small Python utilities that listen to the board over USB serial.
- `Resources/` – schematics, pin maps, and other reference material.
- `tools/` – helper shell scripts for deployment (`deploy_and_monitor.sh`) and resets.

## Choosing what runs on boot
`code.py` now stays minimal; flip the `ACTIVE_MODE` constant when you want to run a
different script on the board. The default production scanner is `controller`.

Available run modes live near the top of `code.py`. From a REPL you can run:

```python
import code
print(code.available_modes())
```

Notable entries:
- `controller` – main 64-channel sensor → MIDI bridge.
- `controller:f0-debug` – runs the I2S pitch tracker loop instead of sensors.
- `heartbeat`, `single-sensor`, `sensor-bank`, `big-sensor-scan`, … – legacy bring-up tools retained for quick bench work.
- `big-sensor-scope` – sweeps sensors, then streams oscilloscope-style captures and I2S STFT data.

## Deploying to a board
Either drag the `src/` and `assets/` directories onto the mounted `CIRCUITPY`
volume, or use the helper script:

```bash
cd midi_controller_embedded_sw
tools/deploy_and_monitor.sh   # deploys, tails the serial log
```

Examples:
```bash
# Main board
cd midi_controller_embedded_sw
tools/deploy_and_monitor.sh --main

# Sensor board with id 1 
cd midi_controller_embedded_sw
tools/deploy_and_monitor.sh --sensor_node_id 1
```

The script:
1. Performs a REPL break/reset (so the board reloads cleanly),
2. Syncs `src/` (and `assets/` if present) onto the drive,
3. Re-attaches a serial monitor so you can watch log output immediately.

Set `MODE=prod` when you want `boot_prod.py` copied instead of the default dev boot file.

## Pin configuration
- Edit `config/pins.json` to adjust mux selects, ADC channels, UART/I2C breakouts, or status LEDs without touching the Python sources.
- The board reads that file on boot via `app.helpers.config.pins()`. From a REPL you can call `from app.helpers.config import reload_pins; reload_pins()` after editing to pick up changes.
- The deploy script now syncs the `config/` directory automatically; double-check the log output if your edits are not appearing on the device.

## Sensor calibration & masking
- `app/helpers/sensors.py` defines `MASKED_SENSORS`, polarities, and hysteresis tuning. Adjust
  these constants per board revision, then redeploy.
- `print_table()` (triggered every `PRINT_EVERY_SWEEPS` iterations) dumps a concise
  view of min/max ranges, thresholds, and enable flags so you can sanity-check the calibration.

## Host-side tips
- `host_code/listen_for_single_sensor_high_res.py` is handy when tuning a single IR pair.
- All runtime logs begin with `# LOG` / `# NOTE`, making it easy to filter with `rg`, `grep`, or the host scripts.

## USB serial debug keys
When USB serial is connected (and `usb_verbose` is enabled for RS-485 sensor nodes), the following key presses are handled. Commands are case-insensitive.
- `Enter`: print the sensor table (main host prints the full multi-node table; sensor node prints its local table).
- `R`: enter calibration mode (main host reboots nodes into calibration mode; sensor node switches into its calibration loop).
- `S`: save calibration + clear the reset-calibration flag, then reboot (also re-enables MSC on next boot).
- `X`: exit calibration without saving and reboot (sensor node only).

## Contributing / development workflow
- Consolidated helpers live in `app/helpers/utils.py`, `app/helpers/midi_play.py`, and `app/helpers/sensors.py`.
- Shared GPIO handles are managed through `app/helpers/hardware.py`; call its helpers (`claim_output`, `claim_input`) instead of instantiating `DigitalInOut` directly so multiple run modes can coexist without "pin in use" errors.
- New experiments can be dropped into `app/` and registered in `code.RUN_MODES`
  so they are selectable without editing the main control loop.
- Keep comments concise and focused on hardware assumptions or non-obvious timing constraints.
