#include "core1/scan.h"

#include <string.h>

#include "pico/time.h"

#include "board_config.h"
#include "core1/events.h"
#include "core1/tla2518.h"
#include "ipc/rings.h"

photon_scan_ctl_t g_scan_ctl;

static uint16_t readings[PHOTON_MAX_SENSORS];

static const uint8_t slot_adc_channel[PHOTON_SLOTS_PER_BANK] = PHOTON_SLOT_ADC_CHANNELS;
static const uint8_t slot_emitter_bit[PHOTON_SLOTS_PER_BANK] = PHOTON_SLOT_EMITTER_BITS;

static inline uint8_t emitter_mask(int slot) {
    return (uint8_t)(1u << slot_emitter_bit[slot]);
}

// Per-(bank, slot) emitter mask: 0 means "never light this position" for
// slots past the populated count or masked out by config. Skipping the GPO
// writes saves emitter current (and two SPI frames) on every sweep.
static uint8_t bank_slot_mask[PHOTON_BANK_COUNT][PHOTON_SLOTS_PER_BANK];
static uint32_t emitter_mask_source = 0xFFFFFFFFu;  // forces the first build

static void refresh_emitter_masks(void) {
    uint32_t disabled = g_events.disabled_mask;
    if (disabled == emitter_mask_source) {
        return;
    }
    emitter_mask_source = disabled;
    for (int b = 0; b < PHOTON_BANK_COUNT; b++) {
        for (int s = 0; s < PHOTON_SLOTS_PER_BANK; s++) {
            int idx = b * PHOTON_SLOTS_PER_BANK + s;
            bool skip = idx >= PHOTON_ACTIVE_SENSORS ||
                        ((disabled >> idx) & 1u) != 0;
            bank_slot_mask[b][s] = skip ? 0u : emitter_mask(s);
        }
    }
}

static inline void wait_until_us(uint32_t target) {
    int32_t remain = (int32_t)(target - time_us_32());
    if (remain > 0) {
        busy_wait_us_32((uint32_t)remain);
    }
}

// The proven per-sensor read tail (after LED-on + settle + CHANNEL_SEL):
// one priming frame (manual-mode channel switch data appears at frame N+2),
// (1 << OSR) - 2 accumulation frames, then the result frame.
#define OSR_EXTRA_FRAMES ((1 << PHOTON_OSR_MODE) - 2)

static uint16_t read_slot_single(const tla2518_t *b) {
    (void)tla2518_xfer2(b);  // prime
    for (int k = 0; k < OSR_EXTRA_FRAMES; k++) {
        (void)tla2518_xfer2(b);
    }
    return tla2518_decode(tla2518_xfer2(b), PHOTON_OSR_MODE);
}

static void read_slot_pair(const tla2518_t *ba, const tla2518_t *bb,
                           uint16_t *ra, uint16_t *rb) {
    uint16_t wa, wb;
    tla2518_xfer2_pair(ba, bb, &wa, &wb);  // prime
    for (int k = 0; k < OSR_EXTRA_FRAMES; k++) {
        tla2518_xfer2_pair(ba, bb, &wa, &wb);
    }
    tla2518_xfer2_pair(ba, bb, &wa, &wb);
    *ra = tla2518_decode(wa, PHOTON_OSR_MODE);
    *rb = tla2518_decode(wb, PHOTON_OSR_MODE);
}

// --------------------------------------------------------------------------
// Mode: SEQUENTIAL (v1 parity)
// --------------------------------------------------------------------------

static void sweep_sequential(void) {
    for (int bank = 0; bank < PHOTON_BANK_COUNT; bank++) {
        tla2518_t *b = &g_banks[bank];
        if (!b->present) {
            continue;
        }
        for (int slot = 0; slot < PHOTON_SLOTS_PER_BANK; slot++) {
            uint8_t mask = bank_slot_mask[bank][slot];
            if (mask) {
                tla2518_emitters(b, mask, true);
            }
            // CHANNEL_SEL under the settle window, matching step_banks(): the
            // ADC mux must sit connected to the new channel while the source
            // settles. The original ordering (settle first, chsel, read
            // immediately) gave the mux zero on-channel time and produced
            // wrong readings on real hardware — worst on bank 7 slot 0,
            // whose mux parks on the floating unpopulated AIN1 (idx 31)
            // through the inter-sweep gap and drifts to mid-rail (~32k).
            // Mode 0 is a benchmarking reference (zero optical crosstalk by
            // construction), not a production mode; see firmware/README.md.
            uint32_t settle_done = time_us_32() + g_scan_ctl.settle_us;
            tla2518_write3(b, TLA_OP_REGISTER_WRITE, TLA_REG_CHANNEL_SEL,
                           slot_adc_channel[slot] & 0x0F);
            wait_until_us(settle_done);
            readings[bank * PHOTON_SLOTS_PER_BANK + slot] = read_slot_single(b);
            if (mask) {
                tla2518_emitters(b, mask, false);
            }
        }
    }
}

// --------------------------------------------------------------------------
// Modes: PARALLEL and TWO_PHASE. One "step" lights slot s on a set of
// banks, shares a single settle interval (CHANNEL_SEL writes hidden under
// it), then reads bus-A/bus-B banks pairwise so both SPI peripherals clock
// concurrently.
// --------------------------------------------------------------------------

static void step_banks(const uint8_t *bank_list, int nbanks, int slot) {
    // 1. LEDs on (skipping masked/unpopulated positions).
    for (int i = 0; i < nbanks; i++) {
        tla2518_t *b = &g_banks[bank_list[i]];
        uint8_t mask = bank_slot_mask[bank_list[i]][slot];
        if (b->present && mask) {
            tla2518_emitters(b, mask, true);
        }
    }
    uint32_t settle_done = time_us_32() + g_scan_ctl.settle_us;
    // 2. Channel selects, hidden under the settle window.
    for (int i = 0; i < nbanks; i++) {
        tla2518_t *b = &g_banks[bank_list[i]];
        if (b->present) {
            tla2518_write3(b, TLA_OP_REGISTER_WRITE, TLA_REG_CHANNEL_SEL,
                           slot_adc_channel[slot] & 0x0F);
        }
    }
    // 3. Remaining settle.
    wait_until_us(settle_done);
    // 4. Reads: pair bus-A bank with its bus-B partner where both exist.
    //    bank_list is ordered so entry i pairs with entry i + nbanks/2.
    int half = nbanks / 2;
    for (int i = 0; i < half; i++) {
        tla2518_t *ba = &g_banks[bank_list[i]];
        tla2518_t *bb = &g_banks[bank_list[i + half]];
        int ia = bank_list[i] * PHOTON_SLOTS_PER_BANK + slot;
        int ib = bank_list[i + half] * PHOTON_SLOTS_PER_BANK + slot;
        if (ba->present && bb->present) {
            read_slot_pair(ba, bb, &readings[ia], &readings[ib]);
        } else if (ba->present) {
            readings[ia] = read_slot_single(ba);
        } else if (bb->present) {
            readings[ib] = read_slot_single(bb);
        }
    }
    // 5. LEDs off.
    for (int i = 0; i < nbanks; i++) {
        tla2518_t *b = &g_banks[bank_list[i]];
        uint8_t mask = bank_slot_mask[bank_list[i]][slot];
        if (b->present && mask) {
            tla2518_emitters(b, mask, false);
        }
    }
}

static void sweep_parallel(void) {
    // Bus A banks 0-3 pair with bus B banks 4-7.
    static const uint8_t all_banks[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    for (int slot = 0; slot < PHOTON_SLOTS_PER_BANK; slot++) {
        step_banks(all_banks, 8, slot);
    }
}

static void sweep_two_phase(void) {
    // Even banks then odd banks: doubles the minimum lit-emitter spacing.
    static const uint8_t even_banks[4] = { 0, 2, 4, 6 };
    static const uint8_t odd_banks[4] = { 1, 3, 5, 7 };
    for (int slot = 0; slot < PHOTON_SLOTS_PER_BANK; slot++) {
        step_banks(even_banks, 4, slot);
        step_banks(odd_banks, 4, slot);
    }
}

static void sweep(void) {
    refresh_emitter_masks();
    switch (g_scan_ctl.mode) {
        case PHOTON_SCAN_PARALLEL:
            sweep_parallel();
            break;
        case PHOTON_SCAN_TWO_PHASE:
            sweep_two_phase();
            break;
        default:
            sweep_sequential();
            break;
    }
}

// --------------------------------------------------------------------------
// Startup robustness: all-zero sweeps right after bring-up indicate the
// first-transaction SPI glitch seen in the field on CircuitPython 10.1.x;
// re-reset the banks (up to 3 attempts) before giving up to a live loop.
// --------------------------------------------------------------------------

static bool sweep_all_zero(void) {
    for (int bank = 0; bank < PHOTON_BANK_COUNT; bank++) {
        if (!g_banks[bank].present) {
            continue;
        }
        for (int slot = 0; slot < PHOTON_SLOTS_PER_BANK; slot++) {
            if (readings[bank * PHOTON_SLOTS_PER_BANK + slot] != 0) {
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Pseudorandom event generator (bus/loss validation instrument): injects
// synthetic note events at a configured rate with stateful ON/OFF pairing,
// so per-event sequence accounting at the bridge proves zero loss under
// sustained load. xorshift32, deterministic per boot.
// ---------------------------------------------------------------------------

static struct {
    uint32_t rate;        // events/sec, 0 = off
    uint32_t credit_fp16; // fractional events accumulated, <<16
    uint32_t prng;
    uint32_t held_mask;   // synthetic keys currently ON
} test_gen;

static inline uint32_t prng_next(void) {
    uint32_t x = test_gen.prng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    test_gen.prng = x;
    return x;
}

static void test_gen_tick(uint32_t period_us, uint32_t now_us) {
    if (test_gen.rate == 0) {
        return;
    }
    // credit += rate * period (in events, 16.16 fixed point)
    test_gen.credit_fp16 += (uint32_t)(((uint64_t)test_gen.rate * period_us << 16) / 1000000u);
    while (test_gen.credit_fp16 >= (1u << 16)) {
        test_gen.credit_fp16 -= 1u << 16;
        uint32_t r = prng_next();
        uint8_t idx = (uint8_t)(r % PHOTON_ACTIVE_SENSORS);
        uint32_t bit = 1u << idx;
        uint8_t state;
        if (test_gen.held_mask & bit) {
            state = 0;  // release a held key
            test_gen.held_mask &= ~bit;
        } else {
            state = 1;  // press an unheld key
            test_gen.held_mask |= bit;
        }
        uint32_t dt_us = 1000u + ((r >> 8) % 199000u);  // 1..200 ms
        event_ring_push(idx, state, dt_us, now_us);
    }
}

static void drain_mailbox(void) {
    photon_cmd_t cmd;
    while (cmd_mailbox_pop(&cmd)) {
        switch (cmd.op) {
            case PHOTON_CMD_SET_CAL:
                events_seed_cal(cmd.arg8, (uint16_t)cmd.a, (uint16_t)cmd.b);
                break;
            case PHOTON_CMD_RESET_CAL:
                events_reset_cal();
                break;
            case PHOTON_CMD_CAL_LEARN:
                g_events.learning = cmd.a != 0;
                break;
            case PHOTON_CMD_SETTLE:
                if (cmd.a >= 5 && cmd.a <= 500) {
                    g_scan_ctl.settle_us = (uint16_t)cmd.a;
                }
                break;
            case PHOTON_CMD_SCAN_RATE:
                g_scan_ctl.rate_hz = cmd.a == 0 ? PHOTON_DEFAULT_SCAN_RATE_HZ
                                   : cmd.a >= 0xFFFF ? 0
                                                     : (uint16_t)cmd.a;
                break;
            case PHOTON_CMD_TEST_RATE:
                test_gen.rate = cmd.a;
                test_gen.credit_fp16 = 0;
                if (test_gen.prng == 0) {
                    test_gen.prng = time_us_32() | 1u;
                }
                if (cmd.a == 0) {
                    // Release everything the generator holds so no synthetic
                    // note is left stuck ON at the sinks.
                    uint32_t now = time_us_32();
                    for (uint8_t i = 0; test_gen.held_mask != 0 && i < PHOTON_ACTIVE_SENSORS; i++) {
                        if (test_gen.held_mask & (1u << i)) {
                            event_ring_push(i, 0, 5000, now);
                            test_gen.held_mask &= ~(1u << i);
                        }
                    }
                }
                break;
            case PHOTON_CMD_SET_DISABLED:
                g_events.disabled_mask = cmd.a;
                break;
            case PHOTON_CMD_SCAN_MODE:
                if (cmd.arg8 <= PHOTON_SCAN_TWO_PHASE) {
                    g_scan_ctl.mode = cmd.arg8;
                }
                break;
            case PHOTON_CMD_TRACE_TAP:
                g_scan_ctl.trace_idx = cmd.arg8;
                g_scan_ctl.trace_enabled = cmd.a != 0;
                break;
            case PHOTON_CMD_PARK:
                cmd_mailbox_park_here();
                break;
            case PHOTON_CMD_TEST_BURST: {
                // Synthetic events through the full event path (M3/M5
                // torture instrument). Alternating ON/OFF over the sensor
                // range so the bridge also exercises note arbitration.
                uint32_t now = time_us_32();
                for (uint32_t k = 0; k < cmd.a; k++) {
                    event_ring_push((uint8_t)(k % PHOTON_ACTIVE_SENSORS),
                                    (uint8_t)((k / PHOTON_ACTIVE_SENSORS) & 1u ? 0 : 1),
                                    5000, now);
                }
                break;
            }
            default:
                break;
        }
    }
}

void scan_core1_entry(void) {
    if (g_scan_ctl.settle_us == 0) {
        g_scan_ctl.settle_us = PHOTON_SETTLE_US;  // compile-time default
    }
    // Field-proven bring-up ritual (CircuitPython main.py had the same two
    // layers for a first-transaction SPI glitch): up to 3 reset attempts
    // with settle gaps, then a ~1 s in-service check; if the array still
    // reads all-zero, raise zero_fault so core 0 can escalate (reboot when
    // no console is attached — the legacy microcontroller.reset() behavior).
    tla2518_reset_all();
    for (int attempt = 0; attempt < 3; attempt++) {
        sweep();
        if (!sweep_all_zero()) {
            break;
        }
        g_scan_ctl.reinit_count++;
        busy_wait_us_32(50000);
        tla2518_reset_all();
    }

    uint32_t boot_t0 = time_us_32();
    bool boot_checked = false;
    uint32_t prev_t0 = time_us_32();
    uint32_t next_sweep_at = time_us_32();

    for (;;) {
        // Pacing: absolute schedule so the period doesn't drift; emitters
        // stay dark for the whole idle gap. Overruns resync to now.
        uint16_t rate = g_scan_ctl.rate_hz;
        if (rate != 0) {
            uint32_t period = 1000000u / rate;
            wait_until_us(next_sweep_at);
            uint32_t now = time_us_32();
            next_sweep_at += period;
            if ((int32_t)(next_sweep_at - now) < 0) {
                next_sweep_at = now + period;
            }
        }
        uint32_t t0 = time_us_32();
        g_scan_ctl.period_us = t0 - prev_t0;
        prev_t0 = t0;
        sweep();
        events_process(readings, t0);
        if (g_scan_ctl.trace_enabled) {
            uint8_t ti = g_scan_ctl.trace_idx;
            if (ti < PHOTON_MAX_SENSORS) {
                trace_ring_push(t0, readings[ti]);
            }
        }
        g_scan_ctl.sweep_count++;
        g_scan_ctl.sweep_us = time_us_32() - t0;
        test_gen_tick(g_scan_ctl.period_us, t0);
        snapshot_publish(g_events.value, g_events.min, g_events.max,
                         g_events.var_ema, g_scan_ctl.sweep_count,
                         g_scan_ctl.sweep_us);
        drain_mailbox();

        if (!boot_checked && (time_us_32() - boot_t0) > 1000000u) {
            boot_checked = true;
            if (sweep_all_zero()) {
                g_scan_ctl.zero_fault = true;
            }
        }
    }
}
