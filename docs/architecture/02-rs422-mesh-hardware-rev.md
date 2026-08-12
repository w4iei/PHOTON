# PHOTON RS-422 Node-Node Mesh (Plan 2 — next hardware revision)

Status: **plan only — no firmware or hardware implementation yet.** This document specifies the
next sensor-board revision and the transport layer that replaces the shared RS-485 bus. It builds
directly on the native firmware of [01-native-dual-core-firmware.md](01-native-dual-core-firmware.md):
everything above the `transport.h` interface (scan core, event engine, bridge role, config store,
frame format v2) carries over unchanged.

## 1. Concept

Replace the single shared half-duplex RS-485 bus with **point-to-point full-duplex RS-422 links
between neighboring boards** ("Left" and "Right" ports). Boards form a linear chain; frames are
store-and-forwarded toward their destination in both directions. There is no shared medium, so
there is no contention — the collision class of bugs is eliminated *physically* rather than by
bus scheduling.

- Every node runs the identical firmware and carries full system knowledge (note map, velocity
  curve, calibration); **any node that enumerates on a USB host becomes the bridge**.
- A **vastly simplified endpoint node** (RP2350 + 2× RS-422 ports + USB-C, no sensors) lives
  outside the instrument and is the usual bridge. It runs the same UF2; with no TLA2518s found,
  core 1 is simply never launched. This solves the "a USB cable can't gracefully exit the
  harpsichord" problem: only a thin 6-wire RS-422 cable (~50 cm) crosses the case; board-board
  hops inside are 5–20 cm.
- The main controller board as a distinct design **disappears** from the system.

## 2. Physical layer

### 2.1 Transceivers
- **2× THVD1424 per board** (same part as today, one per port), strapped **full-duplex** via the
  H/F pin. Driver and receiver both always enabled (DE tied high, RE tied low) — a dedicated
  pair per direction needs no direction switching, which also removes the 25 µs DE guard bands
  from the timing budget.
- Freed RP2350 pins: GPIO24 (was DE), GPIO25 (was TERM).
- **Termination**: fixed 120 Ω across each *receiver* pair (RS-422 point-to-point: exactly one
  termination at each receiving end). The per-connector termination DIP switches and the
  firmware-controlled TERM line are deleted.
- **Failsafe bias**: the current RS-485 rev has no pull-up/pull-down bias network on the bus;
  it works only because the THVD1424's internal receiver failsafe reads an idle/open bus as a
  defined high (bench-proven: zero hdr/crc errors across millions of idle-gap turnarounds at
  4 Mbaud). Full-duplex RS-422 pairs are driven continuously (no idle windows), so bias
  matters less here — but if any pair can be undriven (unplugged link, endpoint off), add the
  standard bias as cheap insurance: ~560 Ω pull-up on A to 3.3 V and pull-down on B to GND at
  one point per pair, or explicitly note reliance on the internal failsafe.
- Slew-limited mode is unnecessary at these lengths; run the THVD1424 in its fast (20 Mbps) mode.

### 2.2 UART assignment (RP2350, both are hardware UARTs — verified free)
| Port | UART | Pins |
|---|---|---|
| Left | UART1 (F11 alt mapping, as today) | TX GPIO22, RX GPIO23 |
| Right | UART0 (F2 mapping) | TX GPIO16, RX GPIO17 (alternate: GPIO12/13) |

USB + UART0 + UART1 are independent peripherals with independent IRQs and DMA DREQs — verified
no structural conflict. Baud: **4 Mbaud** (exact divisor at clk_peri = 150 MHz: IBRD 2, FBRD 22;
UART ceiling is clk_peri/16 = 9.375 Mbaud; THVD1424 fast mode is 20 Mbps).

### 2.3 Connector and cable
**6-pin JST-SH per port** (up from 4): `1 V+ | 2 GND | 3 TX+ | 4 TX− | 5 RX+ | 6 RX−`.
The Right connector's TX/RX positions are **mirrored** relative to Left, so every cable is an
identical straight-through 6-wire — no crossover cables to mislabel.

### 2.4 Power daisy-chain budget
Cables carry power (user requirement). Worst case per board with parallel lighting: 8 LEDs ×
27 mA ≈ 216 mA + RP2350/ADCs ≈ **~350 mA peak**; a 4-board chain ⇒ 1.2–1.4 A through the first
cable — at or above JST-SH per-pin comfort. Mitigations to design in:
- Per-board bulk capacitance sized for the ms-scale LED bursts (bursts across boards are
  decorrelated by the scan phase offsets).
- The 2-phase scan mode halves per-board peak draw (runtime-selectable already).
- Provision **mid-chain power injection** (the 6-pin pinout allows feeding V+ at any node).
- The endpoint node takes **external power** (USB 500 mA alone cannot feed a long chain);
  back-power diode/jumper arrangement as on rev D.

### 2.5 PSRAM (optional, DNP by default)
RP2350A QFN-60 QMI CS1n choices are **GPIO0 / 8 / 19 only** — all three currently taken
(SPI0 RX / SPI1 RX / bank-2 CS). Least-cost change: **move bank-2 CS GPIO19 → GPIO14** (free);
PSRAM CS = GPIO19. Firmware rule: PSRAM is XIP-cached (jitter) → **core 1 never touches it**;
its only sanctioned use is core-0 deep raw-trace capture (e.g. tens of seconds of full-array
data at kHz rates). Not required for any core function — 520 KB SRAM covers all queues ~10×.

### 2.6 Silicon
Target the **A4 stepping** (E9 pull-down erratum fixed, July 2025) for new fabrication.

## 3. Link and routing layer (replaces `rs485_bus.c` behind `transport.h`)

New firmware modules (future work, not written now): `link.c` (per-port UART+DMA driver),
`arq.c` (reliability), `router.c` (store-and-forward + enumeration).

### 3.1 Frame format
Identical **frame v2** from Plan 1 (`A5 5A | type | flags | src | dst | len | seq u16 | hdr_crc8
| payload | crc32`). `flags.DIR` becomes meaningful: 0 = toward bridge, 1 = away. `src`/`dst`
are **position addresses** (below). `seq` is link-local per direction.

### 3.2 Addressing: hop-count enumeration (replaces `setid`)
- On bridge activation (USB mount), the node takes address 0 and emits `ENUM{hop=0}` on both
  ports.
- A node receiving `ENUM` on port P records `uplink = P`, takes `my_addr = hop+1`, re-emits
  `ENUM{hop+1}` on its other port. No live neighbor there (no `LPROBE` heartbeat for 300 ms) →
  it is the terminus and returns `ENUM_END{count}` up the chain.
- Routing is then trivial: DIR=toward-bridge → uplink; DIR=away → consume if `dst == my_addr`,
  else forward on downlink. Broadcast-away floods the downlink.
- Re-enumeration on: bridge activation, any link up/down (heartbeat loss). A second node
  mounting USB while a bridge exists logs a conflict and stays passive (single-bridge
  assumption; deterministic).
- Flash `node_id` and the `setid` console command are retired; per-node calibration is keyed by
  a board serial (`pico_get_unique_board_id`) so it survives re-enumeration and re-ordering.

### 3.3 Reliability: per-hop Go-Back-N + credit flow control
Chosen over end-to-end ARQ: links are private point-to-point (contention is gone by
construction; the only error source is line noise), so retransmit state stays local and small.
"Zero loss" decomposes into (a) CRC catches corruption → GBN repairs it, and (b) queues cannot
overflow → credit forbids it (backpressure propagates instead of dropping).

Per link, per direction:
- Retransmit ring **64 frames** (64 × 144 B = 9.2 KB); sender limited to `credit` unacked frames.
- `LACK{acked_seq, credit}` after every 4 data frames or 250 µs after first unacked frame;
  `credit` = free slots in the receiver's forward FIFO.
- Gap or CRC failure → `LNAK{last_good_seq}` → sender rewinds (Go-Back-N). Sender backstop
  timeout 1 ms. No retry cap; a link dead > 100 ms raises `LINK_STATUS` toward the bridge and
  a console alarm — mid-chain death makes downstream unreachable, so it must be loud, never
  silent.
- `LACK`/`LNAK`/`LPROBE` (100 ms idle heartbeat) are link-local: never routed, never acked.

### 3.4 Store-and-forward queues and priority
Per output port one FIFO of **128 × 144 B slots** (18.4 KB; both ports ≈ 37 KB — trivial in
520 KB SRAM). Strict priority: PRIO_EVENT frames (event batches, credit-carrying LACKs) preempt
bulk (TRACE/DATA) in the scheduler — trace streaming can never delay a note.

### 3.5 Latency budget (4 Mbaud, 4-hop chain: 3 sensor boards + endpoint)
| Case | Result |
|---|---|
| 32 simultaneous events (batched 12/frame → 3 frames, 362 B) | ≈ 0.9 ms per link serialization; ≤ 1 store-and-forward frame-time penalty per hop; **last event at bridge ≈ 2.0 ms** |
| First event at bridge | ≈ 0.2 ms |
| 61-key "doomsday" chord | ≈ 2.9 ms end-to-end; ≤ 61 of 128 queue slots — credit never engages |

### 3.6 Flow mapping
EVT_POLL disappears (nodes push again — safely, on private links). EVENT batches flow
toward-bridge with PRIO_EVENT. PING/DATA/MINMAX/STATS/TRACE/CAL become addressed
request/response over the routed chain, unchanged above the transport. `TEST_BURST` remains the
torture instrument.

### 3.7 Time synchronization: not needed
Velocity `dt` is measured between two threshold crossings on the *same node* with the local µs
clock. Bridge-side ordering is arrival order; forwarding skew is bounded (≤ ~2 ms in the
doomsday case, ~0.2 ms typical) — far below musical significance. An optional `TIME_BEACON`
(bridge µs counter broadcast; nodes record offsets) is specified only as a diagnostic for
aligning multi-node trace captures; nothing at runtime depends on it.

## 4. Endpoint node (new, minimal board)

RP2350A + W25Q128 + USB-C + 2× THVD1424 (full-duplex) + two 6-pin ports (second port allows
pass-through topologies and power injection) + external power input + A4 stepping. No TLA2518s →
the standard UF2 auto-selects bridge/endpoint role. BOM is a strict subset of the sensor board.

## 5. Migration path from Plan 1 firmware

1. New sensor-board revision + endpoint board fabricated (changes: 2nd THVD1424 + full-duplex
   strapping, 6-pin connectors, UART0 on GPIO16/17, fixed termination, optional PSRAM shuffle,
   power provisions §2.4).
2. Firmware: implement `link.c` / `arq.c` / `router.c` behind the existing `transport.h`;
   delete `rs485_bus.c` + `setid`; add ENUM/LACK/LNAK/LPROBE frame types. Scan core, event
   engine, USB/bridge, config store: **zero changes.**
3. Validation reuses the Plan 1 milestones: loopback fuzz (Left jumpered to Right), 2-board
   soak with saturating `TEST_BURST`, cable-pull GBN recovery, then the M5 chord-burst
   regression on the real instrument.

## 6. Open items to settle at rev time

- Confirm JST-SH current rating vs measured chain draw; consider JST-GH or doubled V+/GND pins
  if the margin is thin.
- Measure RS-422 eye at 4 Mbaud over the actual 50 cm exit cable; drop to 2 Mbaud if marginal
  (config constant).
- Decide PSRAM stuffing default once a concrete deep-trace use case exists.
- Endpoint enclosure/mounting outside the instrument.
