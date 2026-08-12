// Protocol logic for both roles over the transport seam.
//
// Bridge role: sole bus master. Continuous EVT_POLL cycle over alive nodes
// (events arrive as EVT_BATCH replies), one interleaved bulk
// request/response slot between cycles, PING discovery for silent ids.
//
// Node role: strictly reactive — transmits only in reply to a frame
// addressed specifically to it (that is the turn grant). Broadcasts are
// applied but never answered.
#ifndef PHOTON_PROTOCOL_H
#define PHOTON_PROTOCOL_H

#include <stdbool.h>
#include <stdint.h>

#include "comms/frame.h"
#include "ipc/rings.h"

#define PHOTON_BATCH_MAX_EVENTS 10  // (128 - 2) / 12

// STATS_RESP payload (shared with the console decoder).
typedef struct __attribute__((packed)) {
    uint16_t sweep_us;
    uint32_t sweep_count;
    uint32_t events_on;
    uint32_t events_off;
    uint32_t ring_overflows;
    uint32_t crc_errors;
    uint32_t hdr_errors;
    uint8_t scan_mode;
    uint8_t reinit_count;
    uint16_t trace_dropped;
} photon_stats_payload_t;

typedef struct {
    bool alive;
    uint32_t last_seen_us;
    uint32_t polls;
    uint32_t timeouts;
    uint32_t retries;
    uint32_t events_rx;
    uint32_t dup_events;
    uint16_t next_evt_seq;
    bool have_seq;
    uint16_t acked_poll_seq;  // poll seq of the last batch processed
    bool have_ack;
    uint8_t epoch;            // node boot epoch carried in each batch: a
    bool have_epoch;          //   changed epoch = node rebooted, reset seq state
    uint32_t seq_gaps;        // accepted events that skipped sequence numbers
    uint32_t malformed;       // CRC-valid but structurally invalid batches
    uint8_t consecutive_timeouts;
} photon_node_slot_t;

// Delivered (deduplicated) node events land here — the bridge wires this to
// the MIDI mapper; tests wire it to an accounting sink.
typedef void (*protocol_event_sink_t)(uint8_t node_id, const photon_event_t *ev);
// Bulk responses (DATA/MINMAX/STATS/TRACE/acks) — the bridge wires this to
// the console; protocol has no compile-time dependency on the USB front end.
typedef void (*protocol_response_sink_t)(const photon_frame_t *f);
// A node just left the poll cycle (went silent) — the bridge wires this to
// the MIDI mapper so held notes from that node are released.
typedef void (*protocol_node_down_cb_t)(uint8_t node_id);

void protocol_init(bool is_bridge, uint8_t own_addr);
void protocol_set_event_sink(protocol_event_sink_t sink);
// Node role: true while a USB MIDI host is mounted — this board's events
// then go to the local sink instead of waiting for bus polls.
void protocol_set_local_delivery(bool enabled);
void protocol_set_response_sink(protocol_response_sink_t sink);
void protocol_set_node_down_cb(protocol_node_down_cb_t cb);

// Transport RX callback (register with transport_init).
void protocol_on_frame(const photon_frame_t *f);

// Pump: bridge poll scheduler / node trace bookkeeping. Call every loop.
void protocol_task(void);

// Bridge-side console entry points: queue a bulk request toward a node
// (small FIFO; one dispatched per poll cycle). Responses arrive at the
// response sink. Returns false only when the request queue is full.
bool protocol_bridge_request(uint8_t type, uint8_t dst,
                             const uint8_t *payload, uint8_t len);

const photon_node_slot_t *protocol_node_table(void);  // [0..PHOTON_MAX_NODE_ID]
uint32_t protocol_poll_cycles(void);

#endif
