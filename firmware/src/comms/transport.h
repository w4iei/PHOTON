// Transport seam. Plan 1 implements this over the shared half-duplex RS-485
// bus (rs485_bus.c, bridge-mastered polling). The planned RS-422 mesh
// revision replaces the implementation (per-port link + ARQ + router) while
// everything above this interface — protocol handlers, bridge role, scan
// core — stays unchanged. See docs/architecture/02-rs422-mesh-hardware-rev.md.
#ifndef PHOTON_TRANSPORT_H
#define PHOTON_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "comms/frame.h"

typedef void (*transport_rx_cb_t)(const photon_frame_t *frame);

typedef struct {
    uint32_t tx_frames;
    uint32_t rx_frames;
} transport_stats_t;

// use_host_pinout selects the physical wiring (main controller board 001 vs
// sensor board 002) — board identity, today derived from the capability
// probe. terminate drives the bus termination (endpoints only). own_addr is
// the protocol address (0 = bridge).
void transport_init(bool use_host_pinout, bool terminate, uint8_t own_addr,
                    transport_rx_cb_t on_rx);

// Queue a frame. prio=true uses the event-priority queue (poll replies,
// polls); false is bulk (data/trace/cal). Returns false if the queue is full.
bool transport_send(const photon_frame_t *f, bool prio);

// Pump TX state machine + drain RX; call every main-loop iteration.
void transport_task(void);

const transport_stats_t *transport_stats(void);
const frame_parse_stats_t *transport_parse_stats(void);

#endif
