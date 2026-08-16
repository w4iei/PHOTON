# Power-Savings Implementation Plan (Phase A + Phase B)

Status: **approved, ready to execute.** Written to be executed step-by-step
without further design decisions. Follow it in order; every step has its own
verification. Do not improvise beyond what a step says.

## Context (read fully before touching anything)

- Four sensor boards + one bridge on a 4 Mbaud RS-485 ring, two-manual
  harpsichord. The bridge's LDO powers the chain and runs warm.
- Deployed today (tag `full_performance`, commit bda138a): 400 Hz paced scan,
  parallel mode (8 emitters lit at once, ~216 mA peak per board), free-running
  poll cycle (~1,850 rotations/s with 4 nodes).
- Goal: **halve average and peak current** while keeping a 200 Hz scan.
  Numbers: peak 216 → ~108 mA/board (two-phase mode), average emitter current
  ~51 → ~24 mA/board (200 Hz + emitter skip), bridge transmitter duty roughly
  halved (poll throttle).
- Hard constraint: **sensor nodes have no USB access** until a planned service
  round. The deployed node firmware has NO remote rate/mode commands. Phase A
  is bridge-only (bridge has USB). Phase B is one new firmware build for all
  boards, flashed during ONE service round, after which every knob is settable
  over the bus forever.
- Fallback: `git checkout full_performance` builds today's exact behavior.
  Firmware defaults (400 Hz, parallel) are NOT changed by this plan — the new
  operating point is set as per-board persisted config via console commands.

### Known traps (violating these has burned us before)

1. `config_store_save()` on a node stalls it off the bus for ~45–400 ms
   (flash erase with IRQs off). The bridge marks it SILENT and re-discovers
   it via 250 ms pings. This is EXPECTED after any remote save — do not
   debug it, just wait 1 s and re-check `nodes`.
2. The remote stats field `sweep_us` reports the paced sweep PERIOD
   (2,500 µs at 400 Hz), not the scan work time (~590 µs). Do not compute
   emitter duty from it.
3. Changing rate or mode shifts each sensor's electrical offset by
   ±100–570 counts (duty-cycle-dependent). **A full recalibration at the new
   operating point is mandatory at the end** (step B7). Skipping it degrades
   or silences keys.
4. Unknown frame types are ignored by the deployed node firmware
   (verified: `node_handle_frame`'s switch has an empty default). New frame
   types are therefore safe to deploy bridge-first.
5. macOS Finder creates `"<name> 2"` duplicate files/folders in the repo —
   delete on sight, never commit them.
6. Serial ports: the console broker MUST be started with an explicit
   `--port`; a portless broker grabs the first port it sees, including the
   wrong board after a re-enumeration. Port numbers change when cables move.
   Identify boards with `id` (prints role + hw id), never by port number.
7. `picotool load` works; `picotool load -v -x` (combined flags) errors on
   this setup. Use three separate invocations: `load`, `verify`, `reboot`.
8. Foreground `sleep` chains in the harness must run as background tasks.
9. After editing `firmware/src/`, always run BOTH: the firmware build
   (`cmake --build build -j8` in `firmware/`) AND the host tests
   (`ctest --test-dir build` in `firmware/test/host`). Both must pass before
   any flash.

---

## Phase A — bridge-only poll throttling (no node access needed)

### A1. Add the pacing constant

File: `firmware/src/board_config.h`, RS-485 transport section. Add:

```c
// Bridge poll-cycle pacing: minimum period of one full poll rotation over
// all alive nodes. 0 = free-run (legacy). 1000 us halves the bridge
// transmitter duty while keeping worst-case event latency ~1 ms.
#define PHOTON_POLL_CYCLE_US     1000
```

### A2. Pace the rotation in the bridge FSM

File: `firmware/src/comms/protocol.c`. The bridge decides to start the next
poll in its `B_IDLE` state handling inside `protocol_task()` /
`bridge_task()`. Find where the bridge advances to poll the FIRST node of a
new rotation (there is a "next node id" walk; a rotation starts when the walk
wraps back to the lowest alive id).

Implementation:
- Add to the bridge state struct `P`: `absolute_time_t next_cycle_at;`
- When a rotation completes (the node-id walk wraps), set
  `P.next_cycle_at = delayed_by_us(P.next_cycle_at, PHOTON_POLL_CYCLE_US)`;
  if that time is already >1 cycle in the past (bridge fell behind), resync
  it to `make_timeout_time_us(PHOTON_POLL_CYCLE_US)`.
- In `B_IDLE`, before dispatching the first poll of a NEW rotation, return
  early (doing nothing) while `!time_reached(P.next_cycle_at)`.
  **The main loop must keep spinning** — this is a guard condition, not a
  sleep. Bulk requests and discovery pings must still dispatch during the
  idle window (check the existing B_IDLE dispatch order: bulk FIFO first,
  then polls — keep bulk dispatch OUTSIDE the pacing guard).
- `#if PHOTON_POLL_CYCLE_US == 0` (or a runtime check) must preserve today's
  free-running behavior exactly.

### A3. Build, flash the bridge, verify

1. Build + host tests (trap #9).
2. Flash bridge over its USB console: send `bootsel` via the broker TCP port
   (`printf 'bootsel\r' | nc -w 2 localhost 7777`), wait for `/Volumes/RP2350`,
   then `picotool load build/photon.uf2`, `picotool verify build/photon.uf2`,
   `picotool reboot`. The broker reattaches by itself.
3. Verify with `python3 tools/measure.py --port <bridge port> --label "phaseA"`
   — do this only if the instrument may make noise (the load phase plays
   notes); otherwise sample `nodes` twice 10 s apart and compute polls/s.
   **Pass: each node polled at ~1000/s (±10%), gaps=0, malformed=0, and all
   four nodes alive.** Latency check: play keys, notes must feel identical.
4. Commit ("Bridge poll pacing: PHOTON_POLL_CYCLE_US, halves TX duty").

---

## Phase B — node firmware + one service round

Build order matters: implement B1–B3 in the repo, build once, flash all five
boards in one service round (B5), then set the new operating point over the
bus (B6) and recalibrate (B7).

### B1. Remote node control frame (NODECTL)

**Frame type.** `firmware/src/comms/frame.h`:
```c
#define PHOTON_FT_NODECTL    'Z'
```
Payload: `op u8 | arg u16 (LE)`. Ops:
| op | meaning | arg | broadcast allowed? |
|---|---|---|---|
| 0 | reboot (watchdog_reboot) | 0 | NO — addressed only |
| 1 | bootsel (reset_usb_boot) | 0 | NO — addressed only |
| 2 | setid | new id 1–6 | NO — addressed only |
| 3 | set scan rate + save | hz (0=default, 0xFFFF=unthrottled) | yes |
| 4 | set scan mode + save | 0/1/2 | yes |

**Node handler.** `firmware/src/comms/protocol.c`, in the node's frame
switch, mirroring the existing `PHOTON_FT_CAL_SET` handler's shape:
- Reject ops 0–2 unless `f->dst == own address` (not broadcast).
- op 0/1: reply first (send a PONG addressed to the bridge), give the TX
  path time to drain (`sleep_ms(5)` is acceptable here — the board is about
  to reboot anyway), then `watchdog_reboot(0,0,100)` / `reset_usb_boot(0,0)`.
- op 2: `g_config.node_id = arg; config_store_save();` reply PONG. Takes
  effect on next reboot (same semantics as local `setid`).
- op 3: push mailbox cmd `PHOTON_CMD_SCAN_RATE` with the arg (copy the local
  `rate` console command's exact logic, including the 0/0xFFFF handling),
  set `g_config.scan_rate_hz`, `config_store_save()`, reply PONG.
- op 4: push `PHOTON_CMD_SCAN_MODE` (copy local `mode` logic, validate ≤2),
  set `g_config.scan_mode`, `config_store_save()`, reply PONG.
- Remember trap #1: the save makes the node vanish briefly. That is fine.

**Bridge console.** `firmware/src/usb/cdc_console.c`:
- Extend the existing `rate` and `mode` commands: in bridge role,
  `rate <hz> [id]` / `mode <m> [id]` send NODECTL op 3/4 to `<id>` or
  broadcast when omitted (follow the `test` command's dst pattern).
- Add remote forms gated on bridge role: `reboot <id>`, `bootsel <id>`
  (the LOCAL no-argument forms must keep their current behavior),
  and `setid <id> <newid>` (local single-arg form unchanged).
- Update `print_help()` accordingly.

### B2. Emitter skip for unpopulated/disabled slots

File: `firmware/src/core1/scan.c`.
- Today `emitter_mask(slot)` returns one mask used for ALL banks, so all 32
  emitter positions are lit, including index 31 (never read into events) and
  index 30 on boards without a 31st key.
- Add `static uint8_t bank_slot_mask[PHOTON_BANK_COUNT][PHOTON_SLOTS_PER_BANK];`
  and `static void rebuild_emitter_masks(uint32_t disabled_mask)`:
  for each bank b, slot s: `idx = b * PHOTON_SLOTS_PER_BANK + s`; the mask is
  `0` if `idx >= PHOTON_ACTIVE_SENSORS` or `(disabled_mask >> idx) & 1`,
  else the slot's emitter bit (from the existing `emitter_mask(slot)`).
- Call it at scan init with the boot disabled mask, and again when the
  mailbox delivers `PHOTON_CMD_SET_DISABLED` (find that case in scan.c's
  mailbox dispatch; it already stores the mask for the event engine).
- In `step_banks()` and the sequential sweep, replace the shared mask with
  `bank_slot_mask[bank][slot]`; when it is 0, SKIP the two GPO writes
  entirely (saves SPI traffic too). Keep the ADC read (the reading is
  harmless and the tables expect 31 values).
- ~3% saving on boards 1/3, ~6% on boards 2/4 once B5 sets their masks.

### B3. Optional — DO NOT IMPLEMENT unless explicitly asked

WFE-based idle for core 1 between paced sweeps. Real (~10–20 mA/board) but
touches the SRAM-resident timing loop; requires its own validation session.

### B4. Build + tests

Trap #9. Also: `git diff full_performance..HEAD -- firmware/src/comms/frame.h
firmware/src/config/config_store.h` must show ONLY the added `PHOTON_FT_NODECTL`
define (wire/config compatibility proof). Commit before flashing.

### B5. Service round (one USB visit per board, bridge included)

For EACH board, one at a time (never two in BOOTSEL at once):
1. Plug USB. Find the new port (`ls /dev/cu.usbmodem*`). Identify with
   `python3 tools/smoke.py --port <port> id` — match the hw id, do not trust
   port numbers (trap #6). Expected hw ids:
   node 1/2: see `nodes` history; node 3 = 3A041CE43472B6D7,
   node 4 = FC905D6E2686BBFB, bridge = 64B6D6CC17C94893.
2. `python3 tools/smoke.py --port <port> bootsel`, wait for `/Volumes/RP2350`,
   then `picotool load build/photon.uf2 && picotool verify build/photon.uf2
   && picotool reboot` (trap #7).
3. Confirm it came back: `smoke.py --port <port> id` shows the same role,
   addr, and `cfg v<N>` (config preserved).
4. **Boards 2 and 4 only:** their 31st sensor (local index 30) has no key.
   On the board's own console: `disable 30`, then `cal save` (persists the
   mask; calibration is frozen so the save is safe). Verify with the table
   (bare Enter): idx 30 row shows `disabled`.
5. Unplug, next board.

### B6. Set the new operating point (from the bridge console, `nc localhost 7777`)

```
mode 2          # broadcast: two-phase, 4 emitters peak
rate 200        # broadcast: 200 Hz paced
```
Each node saves config (expect brief SILENT blips, trap #1). Verify:
`stats <id>` for each node must show `mode=2` and the sweep period consistent
with 200 Hz (remote sweep_us ≈ 5000, trap #2). Local check on any USB-attached
node would show sweep work ~1.2 ms.

### B7. Recalibrate at the new operating point (MANDATORY — trap #3)

From the bridge console: `cal reset` (broadcast), operator plays every key on
both manuals full-swing, then `cal save`. Verify all four acks, then run the
calibration check: every node ≥ its populated count above the 1360 gate
(node 1: 31, node 2: 30, node 3: 31, node 4: 30 — idx 30 now disabled), and
10 s at rest produces 0 events on every node. Then play-test both manuals.

### B8. Wrap up

- Commit anything outstanding; tag `power_tuned` once B7 passes.
- Measure and record: `nodes` deltas over 60 s (polls/s per node ≈ 1000,
  gaps=0), and if a current meter is available, chain current before/after.
- Update `firmware/README.md` console command list (rate/mode/reboot/bootsel
  remote forms, per-node cal) in the same commit.

## Acceptance criteria (the whole plan)

| Check | Pass |
|---|---|
| Poll rate per node (4 nodes) | ~1000/s, gaps=0, malformed=0 |
| Scan mode / rate, all nodes | mode=2, 200 Hz (period ≈ 5000 µs) |
| Emitters at idx 30/31 | never lit (boards 2/4: idx 30 disabled row) |
| Calibration after B7 | 31/30/31/30 sensors above gate; 0 resting events |
| Playability | both manuals, correct channels (3 upper / 2 lower), no stuck notes |
| Fallback intact | `git checkout full_performance` still builds |

---

## Findings from execution (2026-08-13)

Phase A and Phase B are implemented, flashed to all five boards, and verified.
What the bench work turned up along the way:

### The binding constraint is the regulator, not the firmware

Both board types regulate with **NCP1117LPST33T3G**: 1.0 A, SOT-223, ~1.2 V
dropout (needs ~4.5 V in for 3.3 V out under load), with thermal shutdown. In
the deployed wiring the cables carry the **bridge's 3.3 V rail**, so one
NCP1117 sources the whole four-board chain while each sensor board's own
regulator sits unused.

| Configuration | System power | 3.3 V rail current | vs 1.0 A rating |
|---|---|---|---|
| 400 Hz, parallel (8 emitters, ~216 mA pk/board) | 5 W | ~1.0–1.5 A | **at or over limit** |
| 200 Hz, sequential (1 emitter, ~27 mA pk/board) | 1.75 W | ~0.35–0.53 A | comfortable |

At the original settings the part dissipated 1.7–2.5 W in a package that sheds
about 1 W. The old CircuitPython stack never exposed this because it lit one
emitter at a time — the same profile `mode 0` now restores.

**Failure signature:** sensor ADCs read **saturated (~65,200–65,520 on every
channel)** because the analog rail droops. This is not optical and not
firmware. It migrates between boards as wiring changes, so "board N is broken"
is misleading — suspect the rail first. Diagnose with `data <id>` from the
bridge: saturation across all 31 channels means power, not sensors.

**Recommended hardware fix (not yet done):** carry **5 V** on the cables so
each board's own NCP1117 regulates locally; the parts are already fitted, so
this may be a harness change rather than a respin. Bulk capacitance (~1 mF per
board, sized for the 590 µs emitter pulse at 100 mV droop) helps the pulsed
component but does nothing for DC droop.

### CORRECTION: the emitters are ~5% of system power

Two clean measurements at verified-uniform fleet states (four sensor boards +
bridge, sequential mode) overturned the premise this plan was built on:

| Fleet state | Emitter duty | Measured system power |
|---|---|---|
| 200 Hz sequential | 51% | **3.93 W** |
| 300 Hz sequential | 76% | **4.00 W** |

Solving those two points: **fixed baseline ~3.79 W (95%)**, emitters ~0.14 W at
200 Hz and ~0.21 W at 300 Hz. A 1.5x change in emitter duty moves total power
by 0.07 W. An earlier "1.75 W" reading was an artifact — it was taken while
several boards were halted mid-flash-write.

Where the 4 W actually goes (800 mA at 5 V in):
- **~1.4 W (35%) dissipated as heat in the bridge's LDO** dropping 5 V to 3.3 V.
  Pure loss, and the reason the board runs warm.
- ~2.6 W across five RP2350s at 150 MHz, **forty TLA2518 ADCs** (eight per
  sensor board, permanently powered), and five RS-485 transceivers.

**Consequence for tuning:** scan rate and mode are NOT power levers. Keep
`mode 0` anyway — its value is **peak** current (~27 mA vs ~216 mA), which is
what collapses the 3.3 V rail and caused the ADC-saturation failures; that is a
stability argument, not an energy one. Set the rate for the temporal resolution
you want (300 Hz costs 0.07 W over 200 Hz).

Real power levers, in order of size, none of them scan-related:
1. **LDO dissipation (~1.4 W)** — lower the input voltage toward the dropout
   limit, or use a buck. No firmware change; biggest single win.
2. **MCU clock** — five RP2350s at 150 MHz; `clk_peri` is pinned for the
   4 Mbaud UART divisor, so this needs care.
3. **Core 1 idle** — it busy-spins between paced sweeps (24% of the period at
   300 Hz); a WFE wait would recover some of that.
4. **TLA2518 standby** — forty always-on ADCs is a large share of the baseline;
   they have low-power modes worth investigating.

### Measured scan cost per mode (board 4, at the ADC)

| Mode | Sweep | Peak emitter current | Max usable rate |
|---|---|---|---|
| 0 sequential | 2,536 µs | ~27 mA | ~390 Hz |
| 2 two-phase | 826 µs | ~108 mA | ~1.2 kHz |
| 1 parallel | 585 µs | ~216 mA | ~1.7 kHz |

Sequential is lowest on *both* peak and average: in parallel mode every emitter
stays lit through the other sensors' ADC reads, so most of that emitter-on time
is wasted. It also eliminates optical crosstalk by construction.

### Remaining gap

Disabling a sensor on a node (the local mask that also stops its emitter) is
still USB-only — the bridge's `disable` targets the global note map instead.
Add NODECTL op 5 (set local disabled mask) if a board ever needs masking after
the instrument is closed up.

### DEFECT: sequential mode (mode 0) produces wrong readings — do not use

Found 2026-08-13 during the power-savings rollout. Native sequential mode had
never run on the assembled instrument before (all prior validation was
parallel). On real hardware it reads slots 1-3 elevated (~14-26k where
two-phase/parallel read ~4k at rest) and collapses sensor idx 28 (bank 7
slot 0) to a pinned, noisy ~31k with no key response on ALL boards — which
calibrates into a sliver of range and produces machine-gun retriggering on
that note (A3/E6). The effect is mode-specific: the same board, same firmware,
same settle reads correctly the moment it switches to mode 1 or 2, and
identically again on switching back. Settle value does not fix it (tested
30/45/50/60 us). Mechanism not yet isolated (suspect an intra-bank timing or
adjacent-emitter interaction unique to the one-bank-at-a-time schedule);
until it is diagnosed and bench-verified, mode 0 must not be deployed.
**Candidate fix implemented (commit after c6f784c), PENDING HARDWARE
VALIDATION:** sweep_sequential reordered to write CHANNEL_SEL under the
settle window (matching the proven step_banks ordering) instead of after
it, so the ADC mux gets the settle time connected to the new channel.
Validation recipe: flash one node, A/B `mode 0` vs `mode 2` readings via
`data <id>` — all 31 sensors including idx 28 must converge between modes
at rest and under a held key.

**Validation run 2026-08-13 on board 1: PARTIAL PASS — mode 0 stays
quarantined.** The catastrophic failure is fixed (idx 28: pinned 31,064
with 750-1300 noise -> 10,876 with 240-380 noise), confirming the
mux-settling diagnosis. But the modes still do not converge: 8 sensors
differ >15% (idx 0,1,2,24,25,26,28,30), idx 28 by 44% (mode 0 reads
10,876 vs mode 2's 7,624), and mode 0 remains ~10x noisier.

Leading hypothesis for the residual, untested: mode 0 lights ONE emitter
where mode 2 lights four, so the 3.3 V rail sags less — and on this
hardware the rail is known-marginal (see the NCP1117 section above), so a
systematically higher reading in mode 0 may be partly REAL rather than a
scan defect. That would mean "modes must converge" is the wrong
acceptance test; a better one is whether mode 0 readings are *stable and
repeatable* enough to calibrate against, judged on noise, not on
agreement with mode 2. The 10x noise gap is the part that still needs
explaining either way.

**Production configuration: mode 2 (two-phase), 300 Hz, 50 us settle.**
Two-phase delivers the peak-current goal that motivated sequential (~108 mA
vs parallel's ~216 mA), with 8-key-pitch spacing between simultaneously lit
emitters (better than parallel's 4). Verified: uniform ~3.6-8.4k rest across
all 31 sensors including idx 28 (noise 30 counts vs 750-1300 in mode 0).
System power ~3.9-4.0 W regardless of mode/rate (emitters are ~5%; the
meaningful mode difference is peak current, not energy).
