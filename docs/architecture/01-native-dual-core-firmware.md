# PHOTON Native Dual-Core Firmware (Plan 1 — current hardware)

Status: **approved, in implementation** on branch `arch/sensor-node-mesh-native`.
Scope: replace the CircuitPython stack with bare-metal C (Pico SDK) on the **existing** boards —
sensor board 002 (RS-485 half duplex) and main controller board 001. No hardware changes.
The next-revision RS-422 mesh is specified separately in
[02-rs422-mesh-hardware-rev.md](02-rs422-mesh-hardware-rev.md); this firmware is written so that
revision only replaces the transport layer.

## 1. Why

Two problems drive this rewrite:

1. **Note loss under load (the motivating bug).** With two sensor boards on one manual, fast
   two-hand block chords drop notes. Cause: both nodes push EVENT frames unsolicited on the
   shared half-duplex RS-485 bus; pushes collide with each other and with the host's ACKs, and
   the stop-and-wait retry scheme (12 ms ACK timeout, 4 retries, 64-deep queue) exhausts or
   overflows during bursts. Loss is silent.
2. **Performance ceiling.** CircuitPython owns the main loop on core 0 and core 1 is idle. A full
   ~30-sensor sweep runs ≈250 Hz, and note-velocity timing (`dt`) is quantized to 1 ms — a
   ±12 % velocity error on fast strikes.

Targets for this firmware on unchanged hardware:

| Metric | CircuitPython today | This firmware |
|---|---|---|
| Full-array scan rate | ≈250 Hz | **≥1 kHz** committed (1.4–1.9 kHz estimated) |
| Velocity `dt` resolution | 1 ms | **1 µs** (`time_us_64()`) |
| Event loss under chord bursts | frequent | **zero by construction** (no bus contention) |
| Typical event → bridge latency | 10s of ms under load | **< 1 ms**; ≤ ~2 ms worst-case burst |
| Cores used | 1 | 2 (core 1 = scan, core 0 = comms) |

## 2. Stack decision

**C + Pico SDK (≥ 2.1; developed against the official 2.2.0 tree), bare-metal, no RTOS.**
Chosen over Rust/Embassy after research (Aug 2026): every subsystem here has an official SDK
example (multicore, doorbells, TinyUSB CDC, UART DMA, SPI); the existing custom CircuitPython C
modules (`photon_sensorscan`, `photon_rs485`) already target this SDK underneath, so the proven
scan/event logic ports nearly verbatim; FreeRTOS SMP on RP2350 remains shaky and is unnecessary;
Zephyr lacks core-1 support. Build: `rp2350-arm-s` (Cortex-M33), 150 MHz defaults, `clk_peri`
pinned to 150 MHz for stable UART/SPI dividers.

## 3. Unified firmware image

**One UF2 for every board. No compile-time variants, no CIRCUITPY marker files.**

- **Role by capability probe.** At boot, the firmware attempts a TLA2518 register read on each of
  the 8 bank CS lines. ≥1 bank answers → **sensor role** (core 1 launched). 0 banks → **bridge
  role** (core 1 never launched). The current main controller board therefore runs the same image
  and becomes the bus master/bridge — it is, functionally, the "endpoint node" of the next
  hardware revision one board-rev early.
- **Bridge activation** is gated on `tud_mount_cb` (USB host enumeration completed), not raw
  VBUS, so a charger does not create a bridge.
- **Identity**: each sensor node stores a `node_id` (1–6) in its flash config block, set once via
  the USB console (`setid N`). (The RS-422 revision replaces this with automatic hop-count
  enumeration; on a shared bus there is no topology to derive it from.)

## 4. Core split and memory rules

### Core 1 — scan core (sensor role only)
- Runs the TLA2518 scan pipeline (§6) and the event engine (§7), then services the command
  mailbox once per sweep. Nothing else.
- **100 % SRAM-resident**: every function in its call graph is `__not_in_flash_func`; its stack
  and all data live in SRAM. It never touches flash/XIP, never allocates, and takes no SDK locks.
  Consequence: core-0 flash activity (config commit, USB MSC absent anyway) can never stall a
  sweep, and `multicore_lockout` is satisfied by construction during flash writes (core 1 is
  parked in an SRAM spin, §8).

### Core 0 — everything else
- TinyUSB composite device (CDC console + USB-MIDI), RS-485 transport (§9), protocol handlers
  (§10), config store (§8), and — when in bridge role — velocity mapping + MIDI emission (§11).

### Inter-core plumbing (SPSC rings + doorbells; never the 4-deep SIO FIFO)

| Channel | Direction | Shape | Purpose |
|---|---|---|---|
| `event_ring` | 1 → 0 | 256 × 12 B records | Note events. Core 1 rings **doorbell 0** on enqueue. 256 = 8× the worst 32-event burst. Overflow is counted and alarmed (should be structurally impossible), never silent. |
| `snapshot` | 1 → 0 | double buffer + seqlock, 31 × {value, min, max} u16 | Latest sweep for DATA/MINMAX responses; core 0 reads without ever blocking core 1. |
| `cmd_mailbox` | 0 → 1 | SPSC, 16 × 16 B | Calibration set, disable mask, scan-mode select (parallel / 2-phase / sequential), trace tap, PARK. Polled once per sweep → ≤1 sweep-period command latency, no IRQs into the scan loop. |

Event record (12 B): `local_idx u8 | state u8 (1=ON,0=OFF) | dt_us u32 | t_us u32 | rsvd u16`.
SRAM is uncached on RP2350 → no coherency traps; rings use acquire/release ordering on indices.

## 5. Hardware ground truth (sensor board 002 Rev D)

| Function | Pins |
|---|---|
| RS-485 UART (hardware **UART1**, F11 alt mapping) | TX GPIO22, RX GPIO23 |
| RS-485 DE / termination enable | GPIO24 / GPIO25 |
| "SPI0" bus = hardware **SPI1** | SCLK GPIO10, MOSI GPIO11, MISO GPIO8; CS banks 0–3 = GPIO21, 20, 19, 15 |
| "SPI1" bus = hardware **SPI0** | SCLK GPIO2, MOSI GPIO3, MISO GPIO0; CS banks 4–7 = GPIO1, 7, 5, 6 |

(Bus-name swap: `busio` derived instance from `(sck/8)%2`; native code uses hardware names —
bus A = SPI1/banks 0–3, bus B = SPI0/banks 4–7. Main board: UART1 on GPIO4/5, DE GPIO1, TERM
GPIO18.)

Sensor array: 8 banks × 4 slots of VCNT2025X01; each bank is a TLA2518 whose 4 spare GPIO
channels drive the per-sensor LED emitters (AO3400A, ~27 mA). Slot map per bank: slot 0 → AIN7 /
GPO bit 6, slot 1 → AIN5 / bit 4, slot 2 → AIN3 / bit 2, slot 3 → AIN1 / bit 0.

## 6. Scan pipeline (core 1)

**Unchanged, proven per-sensor sequence** (from `photon_sensorscan`): LED on (`BIT_SET
GPO_VALUE`) → settle 60 µs → `CHANNEL_SEL` write → 1 priming read → 6 OSR dummy reads (OSR = 3,
8× oversample, 16-bit result) → real read → LED off. TLA2518 stays in **manual mode** (auto-
sequence cannot interleave per-sensor GPO writes). SPI 20 MHz mode 0, polled FIFO (frames are
2–3 B; DMA setup would cost more than the transfer). All v1 hot-path debug instrumentation
(~2 100 register reads per sweep) is dropped.

**What changes: banks run concurrently.** The 8 TLA2518s are independent; today they are scanned
strictly sequentially, so the sweep is dominated by 32 × 60 µs of serial settle (~1.9 ms).
Pipelined schedule, per slot-step `s` (0..3), per bus (the two SPI buses run in parallel):

1. LED-on GPO writes for slot `s` on all 4 banks of the bus (~2 µs each).
2. One shared 60 µs settle covers all 4 banks.
3. Read banks sequentially (~30 µs each); as each bank finishes, its slot `s+1` LED is lit so the
   next settle hides under the remaining reads.

Steady state ≈ max(60 µs settle, 4 × 30 µs reads) ≈ 125 µs per step → 4 steps ≈ **500 µs
→ ~1.9 kHz**; without the step-overlap refinement ≈ 720 µs → ~1.4 kHz. Committed: ≥1 kHz.

**Optical crosstalk guard.** Parallel lighting means same-slot emitters in different banks are lit
simultaneously — ≥4 sensor pitches (~55 mm) apart, with the two bus schedules phase-offset by
2 slots so cross-bus lit pairs (bank 3/4 boundary) also keep ≥4 pitches. VCNT2025X01 range is
0.5–3 cm, so 55 mm cross-illumination should be negligible, but it is *measured, not assumed*
(milestone M2): parallel-lit vs strictly-sequential sweeps over identical static geometry; accept
parallel mode iff max per-sensor delta < 1 % of that sensor's calibrated range (and ≪ the
60/40 % hysteresis margin). Runtime-selectable fallbacks (via `cmd_mailbox`, no reflash):
**2-phase** (even banks then odd banks, 8-pitch spacing, ~1 kHz) and **sequential-pipelined**
(one lit LED per bus, settle-bound at 16 × 60 µs ≈ 960 µs ≈ 1 kHz). Even the most conservative
mode meets the target, so crosstalk cannot sink the schedule.

## 7. Event engine (core 1)

Verbatim port of the C `process_scan_events` logic: per-sensor running min/max auto-calibration;
sensor participates only when `range ≥ min_event_range`; two-stage strike state machine (arm at
`strike_pct − strike_window_pct`, fire ON at `strike_pct = 60 %` of range, `dt` = time between
the two crossings) and the symmetric release machine (OFF at `release_pct = 40 %`); per-sensor
polarity support. Deployed constants carry over: `strike_pct 60`, `release_pct 40`,
`activation_pct 3`, `velocity_window_pct 20`, `strike_window_pct 30`, `min_event_range 170`
(×8 at OSR 3 → 1360), `settle_us 60`, `osr_mode 3`.

**Timebase upgrade:** `supervisor_ticks_ms32()` (1 ms) → `time_us_64()` (1 µs). `dt` widens to
u32 µs in the event record; clamping moves to the mapping stage.

## 8. Config store (flash)

Two 4 KB A/B sectors at the top of the 16 MB W25Q128, each: `magic | version u32 | payload |
crc32`. Higher valid version wins; corrupt/blank → compiled defaults + "uncalibrated" console
warning. Payload: `node_id`, per-sensor calibration (min/max), disabled-sensor mask, scan mode,
and the **global** note map + velocity parameters (identical on every node so any node can act
as bridge). Commit sequence: core 0 posts `PARK` via `cmd_mailbox` → core 1 acknowledges and
spins in SRAM → core 0 erases/writes (~50–100 ms, bench-time only) → resume. Replaces
JSON-on-CIRCUITPY and `nvm_flags`.

## 9. Transport: frame v2 + master-polled RS-485

### 9.1 Frame format v2 (wire format shared with the RS-422 revision)

```
A5 5A | type u8 | flags u8 | src u8 | dst u8 | len u8 (≤128) | seq u16 | hdr_crc8
      | payload[len] | crc32 (LE, reflected IEEE — covers type..payload)
```

`flags`: bit0 DIR (reserved for RS-422 routing), bit1 PRIO_EVENT, bits 4–7 protocol version = 2.
Fixes two v1 defects: **CRC32 now covers the header** (v1 covered payload only, so a corrupted
`len`/`type` could pass), and **`hdr_crc8`** (poly 0x07, over `type..seq`) gives O(n)
byte-at-a-time resynchronization (v1 rescanned the buffer per frame — O(n²) on garbage).
Overhead 14 B; single-event frame 24 B; max payload 128 B (DATA_RESP needs 124 B).

### 9.2 Bus discipline: bridge is sole master — collisions eliminated in software

Sensor nodes **never transmit unsolicited**. The bridge runs a continuous cycle:

- `EVT_POLL{dst=n}` (14 B) → node `n` replies one `EVT_BATCH` frame with 0–12 event records
  drained from its `event_ring` (empty reply = 14 B).
- At 2 Mbaud with 25 µs DE guard bands: one empty poll+reply ≈ 200 µs → a 2-node cycle ≈
  400 µs → **typical event latency < 1 ms**. Worst case, 32 events queued on one node: 3 polls
  ≈ 1.7 ms. A saturated 61-note "doomsday" chord across 2 nodes clears in < 3 ms.
- Zero collisions by construction (exactly one transmitter at any time). Line noise is handled by
  per-poll retry (2×, then skip-and-log); CRC-fail/timeout/retry counters are visible via STATS.
  EVENT_ACK from v1 is deleted; instead each EVT_POLL carries the poll-seq of the last batch the
  bridge actually **processed** from that node, and the node releases events from its ring only on
  that explicit ack — a lost reply (even past all retries) just means the same events ride the
  next batch, with the bridge deduplicating by per-event sequence number. No loss window exists.
- Bulk flows (DATA/MINMAX/TRACE/CAL/PING) interleave between poll cycles at lower priority; poll
  replies carry PRIO_EVENT and always win the scheduler.
- Baud stays at the proven 2 Mbaud initially; 4 Mbaud (exact divisor at clk_peri = 150 MHz) is a
  config constant away if the physical bus proves clean.

### 9.3 Driver

`rs485_bus.c` behind the `transport.h` interface (the RS-422 revision swaps in a different
implementation; nothing above the interface changes). UART1 GPIO22/23, 8N1; DE on GPIO24 with
25 µs guards (timer-scheduled, not busy-spun); TERM GPIO25 driven as today. TX = one DMA channel
per frame, TX-done IRQ chains the next queued frame (v1 blocked the CPU for the whole frame).
RX = DMA ring buffer per UART; core 0 polls the write pointer (no per-byte IRQs at 2 Mbaud).

## 10. Protocol flows

| Flow | v2 behaviour |
|---|---|
| `EVT_POLL` / `EVT_BATCH` | §9.2. Batch payload = `count u8` + count × 12 B records + `node_seq u16` for loss accounting. |
| `PING`/`PONG` | Bridge discovery: PING ids 1–6 at boot and when a poll target goes silent; PONG carries role+version. |
| `DATA_REQ`/`DATA_RESP` | Addressed; node returns latest sweep snapshot (31 × {value u16, std/min-max}) from the seqlock buffer. |
| `MINMAX_REQ`/`RESP` | Addressed, chunked; feeds calibration display. (v1's host/node `max_payload` mismatch that silently truncated sensors 30–31 is gone — one shared constant.) |
| `STATS_REQ`/`RESP` | Sweep rate (Hz ×10), event/poll counters, CRC-fail, retries, ring high-water marks. |
| `TRACE_START` / `TRACE_DATA` | Node streams a chosen sensor at full sweep rate in bulk-priority chunks; bridge relays to CDC in the **unchanged** `BEGIN_TRACE …` / `t,adc` / `END_TRACE` ASCII contract, so `software/host_code/listen_for_single_sensor_high_res.py` works unmodified (now at ≥1 kHz). |
| `CAL_SET` / `CAL_COMMIT` / `CAL_ACK` | Broadcast or addressed; values applied to core 1 via mailbox; COMMIT triggers the §8 flash sequence. |
| `TEST_BURST` | Node injects n synthetic events through the full path — the regression instrument for the motivating bug (M3/M5). |

## 11. Bridge role (USB)

- TinyUSB **composite CDC + USB-MIDI**, VID 0x1B4F, new PID 0x0039 (distinct from the
  CircuitPython builds' 0x0038 so host tooling can tell the stacks apart). SDK stdio → CDC only.
- Event path: `EVT_BATCH` → global index = table(node_id, local_idx) → `dt` → velocity
  (8 ms → 127 … 100 ms → 1, curve exponent 2.54 — ported verbatim from `midi_mapping.py`,
  now fed µs-resolution dt) → note arbitration (OR-group per note, as `event_mode.py`) →
  USB-MIDI NoteOn/Off.
- Console commands (any node's CDC, bridge or not): `stats`, `data`, `minmax`, `cal save|reset`,
  `setid N`, `ping N`, `trace N`, `burst N`, `mode parallel|2phase|seq`, `help`.

## 12. Repository layout & port map

```
firmware/
├── CMakeLists.txt, pico_sdk_import.cmake   # PICO_SDK_PATH or fetch; rp2350-arm-s
├── README.md                               # build/flash/debug + bench procedures (M1–M5)
├── src/
│   ├── main.c            # clocks, role probe, core-1 launch, core-0 loop
│   ├── board_config.h    # every pin/baud/depth/threshold (from rs485_system_config.py)
│   ├── core1/  tla2518.{c,h}  scan.{c,h}  events.{c,h}
│   ├── ipc/    rings.{c,h}
│   ├── comms/  frame.{c,h}  transport.h  rs485_bus.{c,h}  protocol.{c,h}
│   ├── usb/    tusb_config.h  usb_descriptors.c  cdc_console.{c,h}  midi_out.{c,h}
│   ├── bridge/ midi_map.{c,h}
│   ├── config/ config_store.{c,h}
│   └── util/   crc32.{c,h}  log.h
└── test/host/            # host-built: deframer fuzz, rings, events replay, poll-cycle sim
```

| Existing source | → Native module | Notes |
|---|---|---|
| `photon_sensorscan` C module (CP fork) | `core1/tla2518.c` + `scan.c` | same register ops; add pipeline; drop debug paths |
| `process_scan_events` (CP fork) | `core1/events.c` | µs timebase; logic verbatim |
| `photon_rs485` C module | `comms/frame.c` + `rs485_bus.c` | v2 frame; do **not** port dead ACK-wait path |
| `rs485_sensor_node/{events,protocol,main}.py` | `comms/protocol.c` + core-0 loop | push+ARQ → polled |
| `usb_console.py`, `trace.py` | `usb/cdc_console.c` | keeps BEGIN_TRACE contract |
| `calibration.py`, `sensor_calibration.py` | `config/config_store.c` | JSON → flash A/B block |
| `rs485_main_host/{event_mode,midi_mapping}.py` | bridge role + `bridge/midi_map.c` | curve/table verbatim |
| `rs485_system_config.py` | `board_config.h` + config defaults | per-node overrides die |

Not ported: `polling_mode.py`, `display.py` (dead), `nvm_flags.py`, marker-file boot logic.
`software/embedded_software/` remains untouched as the parity reference.

## 13. Milestones & verification

| M | Deliverable | Pass criteria |
|---|---|---|
| **M1** | Dual-core skeleton | CDC console shows core-1 counter advancing through `event_ring`; core-0 flash-erase loop does **not** perturb a core-1-toggled GPIO on the scope (proves SRAM residency); `picotool info` shows IMAGE_DEF. |
| **M2** | Scan parity + rate | Unmodified `listen_for_single_sensor_high_res.py` captures a key press identically on CP vs native; sweep ≥1 kHz on a logic analyzer (CS lines); crosstalk experiment run → parallel vs 2-phase decided and recorded here. |
| **M3** | Transport soak | 2 sensor boards + main board; `burst`-driven saturation for hours: STATS seq-accounting shows **zero** event loss; CRC/retry counters behave under cable wiggle. |
| **M4** | End-to-end MIDI | Notes in a DAW; calibration save/load through console; hot data/minmax views. |
| **M5** | **Motivating-bug regression** | Scripted `burst n=32` on all nodes simultaneously × 10⁴: 0 lost (seq-accounted), p99 event→MIDI < 5 ms; then real two-hand block chords vs audio recording — every note present. |

Host-built tests (run on macOS, no hardware): deframer fuzz (noise + embedded frames → 100 %
recovery, O(n) time), ring/seqlock stress, `events.c` replayed against recorded CircuitPython
scan traces → identical ON/OFF/dt decisions, poll-cycle simulator → zero loss at saturation.
