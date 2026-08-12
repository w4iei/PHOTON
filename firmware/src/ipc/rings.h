// Inter-core plumbing. All cross-core state lives in uncached SRAM; the SIO
// FIFO (4 entries on RP2350) is deliberately not used. Three channels:
//
//   event_ring   core1 -> core0   note events, SPSC, polled by core 0's
//                                 always-hot main loop
//   snapshot     core1 -> core0   latest sweep values/min/max, seqlock
//   cmd_mailbox  core0 -> core1   config/cal/mode commands, SPSC,
//                                 polled by core 1 once per sweep
//
// Every function callable from core 1 is a static inline here so it inlines
// into the (SRAM-resident) core-1 call graph and never pulls in flash code.
#ifndef PHOTON_RINGS_H
#define PHOTON_RINGS_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hardware/sync.h"

#include "board_config.h"

// ---------------------------------------------------------------------------
// Event ring
// ---------------------------------------------------------------------------

typedef struct __attribute__((packed)) {
    uint8_t  local_idx;   // 0..31 on this node
    uint8_t  state;       // 1 = note ON, 0 = note OFF
    uint32_t dt_us;       // velocity interval, microseconds
    uint32_t t_us;        // node-local timestamp (truncated us)
    uint16_t seq;         // node-local monotonic event sequence (loss accounting)
} photon_event_t;
_Static_assert(sizeof(photon_event_t) == 12, "event record must be 12 bytes");

typedef struct {
    photon_event_t slots[PHOTON_EVENT_RING_SLOTS];
    volatile uint32_t head;      // consumer index (core 0)
    volatile uint32_t tail;      // producer index (core 1)
    volatile uint32_t overflows; // producer-side drop count (structurally ~impossible)
    volatile uint16_t next_seq;  // producer-owned
} photon_event_ring_t;

extern photon_event_ring_t g_event_ring;

// Core 1 only.
static inline bool event_ring_push(uint8_t local_idx, uint8_t state,
                                   uint32_t dt_us, uint32_t t_us) {
    photon_event_ring_t *r = &g_event_ring;
    if (r->tail - r->head >= PHOTON_EVENT_RING_SLOTS) {
        r->overflows++;
        return false;
    }
    photon_event_t *slot = &r->slots[r->tail % PHOTON_EVENT_RING_SLOTS];
    slot->local_idx = local_idx;
    slot->state = state;
    slot->dt_us = dt_us;
    slot->t_us = t_us;
    slot->seq = r->next_seq++;
    __dmb();
    r->tail = r->tail + 1;
    return true;
}

// Core 0 only: read without consuming (transport re-sends until acknowledged
// by the next poll cycle, then advances with event_ring_release).
static inline uint32_t event_ring_peek(photon_event_t *out, uint32_t max) {
    photon_event_ring_t *r = &g_event_ring;
    uint32_t avail = r->tail - r->head;
    if (avail > max) {
        avail = max;
    }
    __dmb();
    for (uint32_t i = 0; i < avail; i++) {
        out[i] = r->slots[(r->head + i) % PHOTON_EVENT_RING_SLOTS];
    }
    return avail;
}

static inline void event_ring_release(uint32_t n) {
    __dmb();
    g_event_ring.head = g_event_ring.head + n;
}

// ---------------------------------------------------------------------------
// Sweep snapshot (seqlock: core 1 writes after every sweep, core 0 reads
// for DATA/MINMAX responses without ever blocking core 1)
// ---------------------------------------------------------------------------

typedef struct {
    volatile uint32_t seq;  // odd while core 1 is writing
    uint16_t value[PHOTON_MAX_SENSORS];
    uint16_t min[PHOTON_MAX_SENSORS];
    uint16_t max[PHOTON_MAX_SENSORS];
    uint32_t sweep_count;
    uint32_t sweep_us;      // duration of the last sweep
} photon_snapshot_t;

extern photon_snapshot_t g_snapshot;

// Core 1: publish; readings/min/max are the engine's working arrays.
static inline void snapshot_publish(const uint16_t *value, const uint16_t *mn,
                                    const uint16_t *mx, uint32_t sweep_count,
                                    uint32_t sweep_us) {
    photon_snapshot_t *s = &g_snapshot;
    s->seq = s->seq + 1;  // odd: write in progress
    __dmb();
    memcpy(s->value, value, sizeof s->value);
    memcpy(s->min, mn, sizeof s->min);
    memcpy(s->max, mx, sizeof s->max);
    s->sweep_count = sweep_count;
    s->sweep_us = sweep_us;
    __dmb();
    s->seq = s->seq + 1;  // even: stable
}

// Core 0: coherent copy (spins only across a ~30 us memcpy window).
void snapshot_read(photon_snapshot_t *out);

// ---------------------------------------------------------------------------
// Command mailbox core0 -> core1
// ---------------------------------------------------------------------------

typedef enum {
    PHOTON_CMD_NONE = 0,
    PHOTON_CMD_SET_CAL,        // arg8 = sensor idx, a = min, b = max
    PHOTON_CMD_RESET_CAL,      // all sensors -> re-learn from live signal
    PHOTON_CMD_SET_DISABLED,   // a = disabled mask (bit per local sensor)
    PHOTON_CMD_SCAN_MODE,      // arg8 = photon_scan_mode_t
    PHOTON_CMD_TRACE_TAP,      // arg8 = sensor idx, a = enable(1)/disable(0)
    PHOTON_CMD_PARK,           // spin in SRAM until park flag cleared
    PHOTON_CMD_TEST_BURST,     // a = number of synthetic events to inject
} photon_cmd_op_t;

typedef struct {
    uint8_t  op;
    uint8_t  arg8;
    uint16_t _pad;
    uint32_t a;
    uint32_t b;
} photon_cmd_t;

typedef struct {
    photon_cmd_t slots[PHOTON_CMD_MAILBOX_SLOTS];
    volatile uint32_t head;         // consumer (core 1)
    volatile uint32_t tail;         // producer (core 0)
    volatile uint32_t drops;        // failed pushes (mailbox full)
    volatile bool park_requested;   // core 0 sets, core 1 honors
    volatile bool parked;           // core 1 acknowledges
} photon_cmd_mailbox_t;

extern photon_cmd_mailbox_t g_cmd_mailbox;

// Core 0.
bool cmd_mailbox_push(const photon_cmd_t *cmd);

// Core 1, once per sweep.
static inline bool cmd_mailbox_pop(photon_cmd_t *out) {
    photon_cmd_mailbox_t *m = &g_cmd_mailbox;
    if (m->head == m->tail) {
        return false;
    }
    __dmb();
    *out = m->slots[m->head % PHOTON_CMD_MAILBOX_SLOTS];
    __dmb();
    m->head = m->head + 1;
    return true;
}

// Core 1: PARK handshake — spin entirely in SRAM while core 0 writes flash.
static inline void cmd_mailbox_park_here(void) {
    photon_cmd_mailbox_t *m = &g_cmd_mailbox;
    m->parked = true;
    __dmb();
    while (m->park_requested) {
        __dmb();
    }
    m->parked = false;
}

// Core 0: request park, wait for ack; release afterwards.
bool cmd_mailbox_park_request(uint32_t timeout_us);
void cmd_mailbox_park_release(void);

// ---------------------------------------------------------------------------
// Trace ring core1 -> core0: per-sweep samples of one tapped sensor,
// feeding the BEGIN_TRACE/t,adc/END_TRACE CDC contract.
// ---------------------------------------------------------------------------

#define PHOTON_TRACE_RING_SLOTS 1024

typedef struct {
    uint32_t t_us;
    uint16_t value;
    uint16_t _pad;
} photon_trace_sample_t;

typedef struct {
    photon_trace_sample_t slots[PHOTON_TRACE_RING_SLOTS];
    volatile uint32_t head;   // consumer (core 0)
    volatile uint32_t tail;   // producer (core 1)
    volatile uint32_t dropped;
} photon_trace_ring_t;

extern photon_trace_ring_t g_trace_ring;

static inline void trace_ring_push(uint32_t t_us, uint16_t value) {
    photon_trace_ring_t *r = &g_trace_ring;
    if (r->tail - r->head >= PHOTON_TRACE_RING_SLOTS) {
        r->dropped++;
        return;
    }
    photon_trace_sample_t *s = &r->slots[r->tail % PHOTON_TRACE_RING_SLOTS];
    s->t_us = t_us;
    s->value = value;
    __dmb();
    r->tail = r->tail + 1;
}

static inline bool trace_ring_pop(photon_trace_sample_t *out) {
    photon_trace_ring_t *r = &g_trace_ring;
    if (r->head == r->tail) {
        return false;
    }
    __dmb();
    *out = r->slots[r->head % PHOTON_TRACE_RING_SLOTS];
    __dmb();
    r->head = r->head + 1;
    return true;
}

void rings_init(void);

#endif
