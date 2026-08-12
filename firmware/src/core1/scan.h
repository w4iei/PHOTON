// Core-1 scan pipeline. Three runtime-selectable modes (cmd_mailbox):
//
//   SEQUENTIAL — v1-equivalent ordering, one sensor at a time: the parity
//                baseline and the most conservative optical mode (~250 Hz-
//                class timing dominated by 32 serial settles).
//   PARALLEL   — per slot-step, all 8 banks' LEDs lit together, one shared
//                settle, reads paired across the two SPI buses (~1.4 kHz
//                estimated). Lit emitters are >= 4 sensor pitches apart.
//   TWO_PHASE  — even banks then odd banks per step (>= 8 pitch spacing),
//                the crosstalk fallback (~1 kHz).
#ifndef PHOTON_SCAN_H
#define PHOTON_SCAN_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PHOTON_SCAN_SEQUENTIAL = 0,
    PHOTON_SCAN_PARALLEL = 1,
    PHOTON_SCAN_TWO_PHASE = 2,
} photon_scan_mode_t;

// Live control/telemetry block; core 0 reads, core 1 owns.
typedef struct {
    volatile uint32_t sweep_count;
    volatile uint32_t sweep_us;       // active scan time of the last sweep
    volatile uint32_t period_us;      // time between sweep starts (rate = 1/period)
    volatile uint16_t rate_hz;        // pacing target; 0 = unthrottled
    volatile uint32_t reinit_count;   // startup all-zero recovery attempts
    volatile uint8_t  mode;           // photon_scan_mode_t
    volatile uint8_t  trace_idx;
    volatile bool     trace_enabled;
    volatile bool     zero_fault;     // set when the array still reads all-zero
                                      // ~1 s after the reinit attempts; core 0
                                      // escalates (reboot when unattended)
} photon_scan_ctl_t;

extern photon_scan_ctl_t g_scan_ctl;

// Entry point for multicore_launch_core1(); never returns. Assumes
// tla2518_init_and_probe() found >= 1 bank and events_init()/config seeding
// already ran on core 0.
void scan_core1_entry(void);

#endif
