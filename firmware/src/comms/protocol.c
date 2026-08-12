#include "comms/protocol.h"

#include <string.h>

#include "pico/time.h"

#include "board_config.h"
#include "comms/transport.h"
#include "config/config_store.h"
#include "core1/events.h"
#include "core1/scan.h"
#include "util/log.h"

#define BULK_QUEUE_SLOTS 8

static struct {
    bool is_bridge;
    uint8_t own_addr;
    protocol_event_sink_t sink;
    protocol_response_sink_t resp_sink;
    protocol_node_down_cb_t node_down_cb;

    // Node role: batch in flight (peeked, not yet released). Events leave
    // the ring only when a later poll explicitly acknowledges the poll-seq
    // that carried them; a retried poll (same seq) rebuilds the *identical*
    // batch so the ack can never cover events the bridge did not receive.
    uint32_t pending_sent;
    uint16_t pending_poll_seq;
    uint8_t batch_epoch;  // per-boot marker in every batch: lets the bridge
                          // detect a fast node reboot (event seq restart)
    // Local delivery (user rule): USB MIDI host mounted => this node's
    // events go to the local sink; otherwise they wait for bus polls.
    // main.c refreshes this each loop from tud_midi_mounted().
    bool local_delivery;

    // Bridge role.
    photon_node_slot_t nodes[PHOTON_MAX_NODE_ID + 1];
    uint16_t poll_seq;
    uint32_t poll_cycles;
    enum { B_IDLE, B_POLL_WAIT, B_BULK_WAIT } state;
    uint8_t wait_target;
    uint8_t wait_type;         // expected reply type in B_BULK_WAIT
    absolute_time_t wait_deadline;
    uint8_t retry_count;
    uint8_t next_poll_id;
    uint8_t next_ping_id;
    absolute_time_t next_ping_at;
    // console-driven bulk request FIFO, one dispatched per poll cycle
    photon_frame_t bulk_q[BULK_QUEUE_SLOTS];
    uint32_t bulk_head, bulk_tail;
} P;

void protocol_init(bool is_bridge, uint8_t own_addr) {
    memset(&P, 0, sizeof P);
    P.is_bridge = is_bridge;
    P.own_addr = own_addr;
    P.state = B_IDLE;
    P.next_poll_id = 1;
    P.next_ping_id = 1;
    P.next_ping_at = get_absolute_time();
    // Boot epoch: low byte of the boot-time microsecond counter. Crystal
    // and regulator ramp jitter make consecutive boots differ; a collision
    // (1/256) only delays reboot detection until the next seq mismatch.
    P.batch_epoch = (uint8_t)time_us_32();
}

void protocol_set_event_sink(protocol_event_sink_t sink) { P.sink = sink; }
void protocol_set_local_delivery(bool enabled) { P.local_delivery = enabled; }
void protocol_set_response_sink(protocol_response_sink_t sink) { P.resp_sink = sink; }
void protocol_set_node_down_cb(protocol_node_down_cb_t cb) { P.node_down_cb = cb; }

const photon_node_slot_t *protocol_node_table(void) { return P.nodes; }
uint32_t protocol_poll_cycles(void) { return P.poll_cycles; }

// ---------------------------------------------------------------------------
// Shared payload builders
// ---------------------------------------------------------------------------

static void send_reply(uint8_t type, uint8_t dst, uint16_t seq,
                       const uint8_t *payload, uint8_t len, bool prio) {
    photon_frame_t f = { 0 };
    f.type = type;
    f.dst = dst;
    f.len = len;
    f.seq = seq;
    f.flags = prio ? PHOTON_FLAG_PRIO_EVENT : 0;
    if (len > 0) {
        memcpy(f.payload, payload, len);
    }
    transport_send(&f, prio);
}

static void build_stats_payload(photon_stats_payload_t *st) {
    const frame_parse_stats_t *ps = transport_parse_stats();
    st->sweep_us = (uint16_t)(g_scan_ctl.sweep_us > 0xFFFF ? 0xFFFF : g_scan_ctl.sweep_us);
    st->sweep_count = g_scan_ctl.sweep_count;
    st->events_on = g_events.events_on;
    st->events_off = g_events.events_off;
    st->ring_overflows = g_event_ring.overflows;
    st->crc_errors = ps->crc_errors;
    st->hdr_errors = ps->hdr_errors;
    st->scan_mode = g_scan_ctl.mode;
    st->reinit_count = (uint8_t)(g_scan_ctl.reinit_count > 255 ? 255 : g_scan_ctl.reinit_count);
    st->trace_dropped = (uint16_t)(g_trace_ring.dropped > 0xFFFF ? 0xFFFF : g_trace_ring.dropped);
}

// ---------------------------------------------------------------------------
// Node role: strictly reactive. A frame addressed specifically to this node
// is the turn grant; broadcasts apply side effects but are never answered
// (all nodes replying at once would collide on the shared bus).
// ---------------------------------------------------------------------------

static void node_handle_evt_poll(const photon_frame_t *f) {
    uint16_t ack = 0xFFFF;
    if (f->len >= 2) {
        memcpy(&ack, f->payload, 2);
    }
    if (P.pending_sent > 0 && ack == P.pending_poll_seq) {
        event_ring_release(P.pending_sent);
        P.pending_sent = 0;
    }

    // A retry (same poll seq, not acked) must resend the identical batch:
    // cap the peek at the previously sent count so newly pushed events
    // cannot ride a seq the bridge may already have processed.
    uint32_t limit = PHOTON_BATCH_MAX_EVENTS;
    if (P.pending_sent > 0 && f->seq == P.pending_poll_seq) {
        limit = P.pending_sent;
    }
    photon_event_t records[PHOTON_BATCH_MAX_EVENTS];
    uint32_t n = event_ring_peek(records, limit);
    P.pending_sent = n;
    P.pending_poll_seq = f->seq;

    uint8_t payload[2 + PHOTON_BATCH_MAX_EVENTS * sizeof(photon_event_t)];
    payload[0] = (uint8_t)n;
    payload[1] = P.batch_epoch;
    memcpy(&payload[2], records, n * sizeof(photon_event_t));
    send_reply(PHOTON_FT_EVT_BATCH, f->src, f->seq, payload,
               (uint8_t)(2 + n * sizeof(photon_event_t)), true);
}

static void node_handle_request(const photon_frame_t *f, bool addressed) {
    switch (f->type) {
        case PHOTON_FT_PING: {
            if (!addressed) {
                break;
            }
            uint8_t info[2] = { 1 /* role: sensor node */, PHOTON_ACTIVE_SENSORS };
            send_reply(PHOTON_FT_PONG, f->src, f->seq, info, sizeof info, true);
            break;
        }
        case PHOTON_FT_DATA_REQ: {
            if (!addressed) {
                break;
            }
            photon_snapshot_t snap;
            snapshot_read(&snap);
            uint8_t payload[1 + PHOTON_ACTIVE_SENSORS * 2];
            payload[0] = PHOTON_ACTIVE_SENSORS;
            memcpy(&payload[1], snap.value, PHOTON_ACTIVE_SENSORS * 2);
            send_reply(PHOTON_FT_DATA_RESP, f->src, f->seq, payload, sizeof payload, false);
            break;
        }
        case PHOTON_FT_MINMAX_REQ: {
            if (!addressed) {
                break;
            }
            photon_snapshot_t snap;
            snapshot_read(&snap);
            uint8_t start = f->len >= 1 ? f->payload[0] : 0;
            uint8_t count = f->len >= 2 ? f->payload[1] : PHOTON_ACTIVE_SENSORS;
            if (start >= PHOTON_ACTIVE_SENSORS) {
                start = 0;
            }
            if (count > PHOTON_ACTIVE_SENSORS - start) {
                count = (uint8_t)(PHOTON_ACTIVE_SENSORS - start);
            }
            uint8_t max_records = (PHOTON_FRAME_MAX_PAYLOAD - 2) / 4;
            if (count > max_records) {
                count = max_records;
            }
            uint8_t payload[PHOTON_FRAME_MAX_PAYLOAD];
            payload[0] = start;
            payload[1] = count;
            for (uint8_t i = 0; i < count; i++) {
                uint16_t mn = snap.min[start + i];
                uint16_t mx = snap.max[start + i];
                memcpy(&payload[2 + i * 4], &mn, 2);
                memcpy(&payload[4 + i * 4], &mx, 2);
            }
            send_reply(PHOTON_FT_MINMAX_RESP, f->src, f->seq, payload,
                       (uint8_t)(2 + count * 4), false);
            break;
        }
        case PHOTON_FT_STATS_REQ: {
            if (!addressed) {
                break;
            }
            photon_stats_payload_t st;
            build_stats_payload(&st);
            send_reply(PHOTON_FT_STATS_RESP, f->src, f->seq,
                       (const uint8_t *)&st, sizeof st, false);
            break;
        }
        case PHOTON_FT_TRACE_START: {
            uint8_t sensor = f->len >= 1 ? f->payload[0] : 0;
            uint8_t enable = f->len >= 2 ? f->payload[1] : 1;
            photon_cmd_t cmd = { .op = PHOTON_CMD_TRACE_TAP, .arg8 = sensor, .a = enable };
            cmd_mailbox_push(&cmd);
            if (addressed) {
                uint8_t ok = 1;
                send_reply(PHOTON_FT_CAL_ACK, f->src, f->seq, &ok, 1, false);
            }
            break;
        }
        case PHOTON_FT_TRACE_DATA: {  // empty request = "pull samples"
            if (!addressed) {
                break;
            }
            uint8_t payload[PHOTON_FRAME_MAX_PAYLOAD];
            uint8_t max_samples = (PHOTON_FRAME_MAX_PAYLOAD - 2) / 6;
            uint8_t n = 0;
            photon_trace_sample_t s;
            while (n < max_samples && trace_ring_pop(&s)) {
                memcpy(&payload[2 + n * 6], &s.t_us, 4);
                memcpy(&payload[6 + n * 6], &s.value, 2);
                n++;
            }
            payload[0] = g_scan_ctl.trace_idx;
            payload[1] = n;
            send_reply(PHOTON_FT_TRACE_DATA, f->src, f->seq, payload,
                       (uint8_t)(2 + n * 6), false);
            break;
        }
        case PHOTON_FT_CAL_SET: {
            if (f->len >= 5) {
                if (f->payload[0] == PHOTON_CAL_IDX_ALL) {
                    photon_cmd_t cmd = { .op = PHOTON_CMD_RESET_CAL };
                    cmd_mailbox_push(&cmd);
                } else {
                    photon_cmd_t cmd = { .op = PHOTON_CMD_SET_CAL, .arg8 = f->payload[0] };
                    uint16_t mn, mx;
                    memcpy(&mn, &f->payload[1], 2);
                    memcpy(&mx, &f->payload[3], 2);
                    cmd.a = mn;
                    cmd.b = mx;
                    cmd_mailbox_push(&cmd);
                }
            }
            if (addressed) {
                uint8_t ok = 1;
                send_reply(PHOTON_FT_CAL_ACK, f->src, f->seq, &ok, 1, false);
            }
            break;
        }
        case PHOTON_FT_CAL_COMMIT: {
            if (!addressed) {
                break;  // never park+flash every node at once off a broadcast
            }
            bool ok = config_store_save();
            uint8_t status = ok ? 1 : 0;
            send_reply(PHOTON_FT_CAL_ACK, f->src, f->seq, &status, 1, false);
            break;
        }
        case PHOTON_FT_TEST_BURST: {
            uint16_t n = 0;
            if (f->len >= 2) {
                memcpy(&n, f->payload, 2);
            }
            photon_cmd_t cmd = { .op = PHOTON_CMD_TEST_BURST, .a = n };
            cmd_mailbox_push(&cmd);
            if (addressed) {
                uint8_t ok = 1;
                send_reply(PHOTON_FT_CAL_ACK, f->src, f->seq, &ok, 1, false);
            }
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Bridge role
// ---------------------------------------------------------------------------

static void bridge_handle_batch(const photon_frame_t *f) {
    photon_node_slot_t *slot = &P.nodes[f->src];
    // Validate structure BEFORE acknowledging anything: a CRC-valid but
    // malformed batch must never trigger an ack that releases node events
    // the bridge did not process.
    uint8_t count = f->len >= 2 ? f->payload[0] : 0;
    if (f->len < 2 || count > PHOTON_BATCH_MAX_EVENTS ||
        (size_t)f->len < 2 + (size_t)count * sizeof(photon_event_t)) {
        slot->malformed++;
        return;
    }
    slot->last_seen_us = time_us_32();
    slot->consecutive_timeouts = 0;

    // A changed boot epoch means the node rebooted (its event seq restarted
    // at 0) — reset sequence tracking so its real events are not discarded
    // as stale duplicates.
    uint8_t epoch = f->payload[1];
    if (!slot->have_epoch || epoch != slot->epoch) {
        slot->epoch = epoch;
        slot->have_epoch = true;
        slot->have_seq = false;
    }

    // Batch fully validated: acknowledge it in the next poll so the node
    // can release the events.
    slot->acked_poll_seq = f->seq;
    slot->have_ack = true;

    for (uint8_t i = 0; i < count; i++) {
        photon_event_t ev;
        memcpy(&ev, &f->payload[2 + i * sizeof ev], sizeof ev);
        if (slot->have_seq && (int16_t)(ev.seq - slot->next_evt_seq) < 0) {
            slot->dup_events++;
            continue;  // retransmitted duplicate
        }
        if (slot->have_seq && ev.seq != slot->next_evt_seq) {
            slot->seq_gaps++;  // should be impossible under ack discipline;
                               // counted for the M5 loss accounting
        }
        slot->have_seq = true;
        slot->next_evt_seq = (uint16_t)(ev.seq + 1);
        slot->events_rx++;
        if (P.sink) {
            P.sink(f->src, &ev);
        }
    }
}

static bool bridge_send_poll(uint8_t node_id, bool retry) {
    if (!retry) {
        P.poll_seq++;
    }
    photon_node_slot_t *slot = &P.nodes[node_id];
    photon_frame_t f = { 0 };
    f.type = PHOTON_FT_EVT_POLL;
    f.dst = node_id;
    f.seq = P.poll_seq;
    f.flags = PHOTON_FLAG_PRIO_EVENT;
    uint16_t ack = slot->have_ack ? slot->acked_poll_seq : 0xFFFF;
    memcpy(f.payload, &ack, 2);
    f.len = 2;
    if (!transport_send(&f, true)) {
        return false;  // TX queue full; caller stays/returns to IDLE and retries
    }
    P.state = B_POLL_WAIT;
    P.wait_target = node_id;
    P.wait_deadline = make_timeout_time_us(PHOTON_POLL_TIMEOUT_US);
    if (!retry) {
        slot->polls++;
    }
    return true;
}

static uint8_t bridge_next_alive(uint8_t from) {
    for (int k = 0; k < PHOTON_MAX_NODE_ID; k++) {
        uint8_t id = (uint8_t)(((from - 1 + k) % PHOTON_MAX_NODE_ID) + 1);
        if (P.nodes[id].alive) {
            return id;
        }
    }
    return 0;
}

bool protocol_bridge_request(uint8_t type, uint8_t dst,
                             const uint8_t *payload, uint8_t len) {
    if (len > PHOTON_FRAME_MAX_PAYLOAD) {
        return false;
    }
    if (P.bulk_tail - P.bulk_head >= BULK_QUEUE_SLOTS) {
        return false;
    }
    photon_frame_t *f = &P.bulk_q[P.bulk_tail % BULK_QUEUE_SLOTS];
    memset(f, 0, sizeof *f);
    f->type = type;
    f->dst = dst;
    f->len = len;
    if (len > 0) {
        memcpy(f->payload, payload, len);
    }
    P.bulk_tail++;
    return true;
}

static uint8_t expected_reply_type(uint8_t req) {
    switch (req) {
        case PHOTON_FT_PING: return PHOTON_FT_PONG;
        case PHOTON_FT_DATA_REQ: return PHOTON_FT_DATA_RESP;
        case PHOTON_FT_MINMAX_REQ: return PHOTON_FT_MINMAX_RESP;
        case PHOTON_FT_STATS_REQ: return PHOTON_FT_STATS_RESP;
        case PHOTON_FT_TRACE_DATA: return PHOTON_FT_TRACE_DATA;
        default: return PHOTON_FT_CAL_ACK;
    }
}

// Dispatch one queued bulk request; returns true if the state machine moved.
static bool bridge_dispatch_bulk(void) {
    if (P.bulk_head == P.bulk_tail) {
        return false;
    }
    photon_frame_t *f = &P.bulk_q[P.bulk_head % BULK_QUEUE_SLOTS];
    P.poll_seq++;
    f->seq = P.poll_seq;
    if (!transport_send(f, false)) {
        return false;  // TX queue full; retry next iteration
    }
    uint8_t dst = f->dst;
    uint8_t type = f->type;
    P.bulk_head++;
    if (dst == PHOTON_ADDR_BROADCAST) {
        return false;  // no reply expected; continue polling immediately
    }
    P.state = B_BULK_WAIT;
    P.wait_target = dst;
    P.wait_type = expected_reply_type(type);
    P.wait_deadline = make_timeout_time_us(2 * PHOTON_POLL_TIMEOUT_US);
    return true;
}

// Round-robin PING to not-yet-alive ids on a fixed cadence, so late-powered
// nodes join the poll cycle whether or not others are already alive.
static bool bridge_maybe_ping(void) {
    if (!time_reached(P.next_ping_at)) {
        return false;
    }
    P.next_ping_at = make_timeout_time_us(PHOTON_PING_INTERVAL_MS * 1000);
    for (int k = 0; k < PHOTON_MAX_NODE_ID; k++) {
        uint8_t id = (uint8_t)(((P.next_ping_id - 1 + k) % PHOTON_MAX_NODE_ID) + 1);
        if (!P.nodes[id].alive) {
            P.next_ping_id = (uint8_t)((id % PHOTON_MAX_NODE_ID) + 1);
            photon_frame_t f = { 0 };
            f.type = PHOTON_FT_PING;
            f.dst = id;
            P.poll_seq++;
            f.seq = P.poll_seq;
            if (!transport_send(&f, true)) {
                return false;
            }
            P.state = B_BULK_WAIT;
            P.wait_target = id;
            P.wait_type = PHOTON_FT_PONG;
            P.wait_deadline = make_timeout_time_us(PHOTON_POLL_TIMEOUT_US);
            return true;
        }
    }
    return false;  // everyone alive
}

static void bridge_task(void) {
    switch (P.state) {
        case B_IDLE: {
            if (bridge_maybe_ping()) {
                return;
            }
            uint8_t first_alive = bridge_next_alive(1);
            uint8_t target = bridge_next_alive(P.next_poll_id);
            // One bulk slot per poll cycle, at the cycle boundary (or
            // whenever no node is pollable).
            if ((target == 0 || target == first_alive) && bridge_dispatch_bulk()) {
                return;
            }
            if (target == 0) {
                return;
            }
            if (target == first_alive) {
                P.poll_cycles++;
            }
            P.next_poll_id = (uint8_t)((target % PHOTON_MAX_NODE_ID) + 1);
            P.retry_count = 0;
            bridge_send_poll(target, false);
            break;
        }
        case B_POLL_WAIT:
            if (time_reached(P.wait_deadline)) {
                photon_node_slot_t *slot = &P.nodes[P.wait_target];
                if (P.retry_count < PHOTON_POLL_RETRIES) {
                    P.retry_count++;
                    slot->retries++;
                    bridge_send_poll(P.wait_target, true);
                } else {
                    slot->timeouts++;
                    if (++slot->consecutive_timeouts >= 8) {
                        slot->alive = false;
                        log_note("node %u silent, dropped from poll cycle", P.wait_target);
                        if (P.node_down_cb) {
                            P.node_down_cb(P.wait_target);
                        }
                    }
                    P.state = B_IDLE;
                }
            }
            break;
        case B_BULK_WAIT:
            if (time_reached(P.wait_deadline)) {
                P.state = B_IDLE;  // request timed out; console reports staleness
            }
            break;
    }
}

// ---------------------------------------------------------------------------

void protocol_on_frame(const photon_frame_t *f) {
    if (!P.is_bridge) {
        bool addressed = f->dst == P.own_addr;
        if (f->type == PHOTON_FT_EVT_POLL) {
            if (addressed) {
                node_handle_evt_poll(f);
            }
        } else {
            node_handle_request(f, addressed);
        }
        return;
    }

    // Bridge side.
    if (f->src > PHOTON_MAX_NODE_ID || f->src == 0) {
        return;
    }
    photon_node_slot_t *slot = &P.nodes[f->src];
    slot->last_seen_us = time_us_32();

    switch (f->type) {
        case PHOTON_FT_EVT_BATCH:
            if (P.state == B_POLL_WAIT && f->src == P.wait_target) {
                P.state = B_IDLE;
            }
            // Deliberately processed even outside a poll-wait window: a
            // batch that arrives after the timeout is a late reply carrying
            // real events, and consuming it now beats waiting for its
            // retransmission next cycle. Safety does not depend on the
            // window — bridge_handle_batch validates structure before
            // acking, dedups by per-event seq, and detects reboots by
            // epoch, so stale or replayed batches cannot corrupt state.
            bridge_handle_batch(f);
            break;
        case PHOTON_FT_PONG:
            if (!slot->alive) {
                slot->alive = true;
                slot->have_seq = false;
                slot->have_ack = false;  // node may have rebooted; stale acks invalid
                slot->consecutive_timeouts = 0;
                log_info("node %u discovered (sensors=%u)",
                         f->src, f->len >= 2 ? f->payload[1] : 0);
            }
            if (P.state == B_BULK_WAIT && f->src == P.wait_target &&
                P.wait_type == PHOTON_FT_PONG) {
                P.state = B_IDLE;
            }
            break;
        default:
            if (P.state == B_BULK_WAIT && f->src == P.wait_target &&
                f->type == P.wait_type) {
                P.state = B_IDLE;
            }
            if (P.resp_sink) {
                P.resp_sink(f);
            }
            break;
    }
}

void protocol_task(void) {
    if (P.is_bridge) {
        bridge_task();
        return;
    }
    // Node local delivery: a mounted USB MIDI host owns this board's
    // events; without one they wait for bus polls. A bridge polling a
    // USB-attached node just sees empty batches — deterministic, USB wins.
    if (P.sink != NULL && P.local_delivery) {
        // Reclaim any batch a bridge never acked; local delivery beats
        // holding events hostage for a poll that may not come.
        P.pending_sent = 0;
        photon_event_t ev;
        while (event_ring_peek(&ev, 1) == 1) {
            P.sink(P.own_addr, &ev);
            event_ring_release(1);
        }
    }
}
