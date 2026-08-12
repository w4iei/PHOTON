#include "comms/protocol.h"

#include <string.h>

#include "pico/time.h"

#include "board_config.h"
#include "comms/transport.h"
#include "config/config_store.h"
#include "core1/events.h"
#include "core1/scan.h"
#include "usb/cdc_console.h"
#include "util/log.h"

static struct {
    bool is_bridge;
    uint8_t own_addr;
    protocol_event_sink_t sink;

    // Node role: batch in flight (peeked, not yet released).
    uint32_t pending_sent;
    uint16_t last_poll_seq;
    bool have_last_poll;

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
    // one pending bulk request slot (console-driven)
    bool bulk_pending;
    photon_frame_t bulk_req;
} P;

void protocol_init(bool is_bridge, uint8_t own_addr) {
    memset(&P, 0, sizeof P);
    P.is_bridge = is_bridge;
    P.own_addr = own_addr;
    P.state = B_IDLE;
    P.next_poll_id = 1;
    P.next_ping_id = 1;
    P.next_ping_at = get_absolute_time();
}

void protocol_set_event_sink(protocol_event_sink_t sink) {
    P.sink = sink;
}

const photon_node_slot_t *protocol_node_table(void) {
    return P.nodes;
}

uint32_t protocol_poll_cycles(void) {
    return P.poll_cycles;
}

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
    const transport_stats_t *ts = transport_stats();
    const frame_parse_stats_t *ps = transport_parse_stats();
    st->sweep_us = (uint16_t)(g_scan_ctl.sweep_us > 0xFFFF ? 0xFFFF : g_scan_ctl.sweep_us);
    st->sweep_count = g_scan_ctl.sweep_count;
    st->events_on = g_events.events_on;
    st->events_off = g_events.events_off;
    st->ring_overflows = g_event_ring.overflows;
    st->crc_errors = ps->crc_errors;
    st->hdr_errors = ps->hdr_errors;
    st->rx_overruns = ts->rx_overruns;
    st->scan_mode = g_scan_ctl.mode;
    st->reinit_count = (uint8_t)(g_scan_ctl.reinit_count > 255 ? 255 : g_scan_ctl.reinit_count);
    st->trace_dropped = (uint16_t)(g_trace_ring.dropped > 0xFFFF ? 0xFFFF : g_trace_ring.dropped);
}

// ---------------------------------------------------------------------------
// Node role: reactive handlers
// ---------------------------------------------------------------------------

static void node_handle_evt_poll(const photon_frame_t *f) {
    // A poll with a new seq implicitly acknowledges the previous batch.
    if (!P.have_last_poll || f->seq != P.last_poll_seq) {
        event_ring_release(P.pending_sent);
        P.pending_sent = 0;
    }
    P.have_last_poll = true;
    P.last_poll_seq = f->seq;

    photon_event_t records[PHOTON_BATCH_MAX_EVENTS];
    uint32_t n = event_ring_peek(records, PHOTON_BATCH_MAX_EVENTS);
    P.pending_sent = n;

    uint8_t payload[2 + PHOTON_BATCH_MAX_EVENTS * sizeof(photon_event_t)];
    payload[0] = (uint8_t)n;
    payload[1] = 0;  // reserved
    memcpy(&payload[2], records, n * sizeof(photon_event_t));
    send_reply(PHOTON_FT_EVT_BATCH, f->src, f->seq, payload,
               (uint8_t)(2 + n * sizeof(photon_event_t)), true);
}

static void node_handle_request(const photon_frame_t *f) {
    switch (f->type) {
        case PHOTON_FT_PING: {
            uint8_t info[2] = { 1 /* role: sensor node */, PHOTON_ACTIVE_SENSORS };
            send_reply(PHOTON_FT_PONG, f->src, f->seq, info, sizeof info, true);
            break;
        }
        case PHOTON_FT_DATA_REQ: {
            photon_snapshot_t snap;
            snapshot_read(&snap);
            uint8_t payload[1 + PHOTON_ACTIVE_SENSORS * 2];
            payload[0] = PHOTON_ACTIVE_SENSORS;
            memcpy(&payload[1], snap.value, PHOTON_ACTIVE_SENSORS * 2);
            send_reply(PHOTON_FT_DATA_RESP, f->src, f->seq, payload, sizeof payload, false);
            break;
        }
        case PHOTON_FT_MINMAX_REQ: {
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
            send_reply(PHOTON_FT_CAL_ACK, f->src, f->seq, &enable, 1, false);
            break;
        }
        case PHOTON_FT_TRACE_DATA: {  // empty request = "pull samples"
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
                if (f->payload[0] == 0xFF) {  // convention: reset all sensors
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
            if (f->dst != PHOTON_ADDR_BROADCAST) {
                uint8_t ok = 1;
                send_reply(PHOTON_FT_CAL_ACK, f->src, f->seq, &ok, 1, false);
            }
            break;
        }
        case PHOTON_FT_CAL_COMMIT: {
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
            if (f->dst != PHOTON_ADDR_BROADCAST) {
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
    photon_node_slot_t *slot = &P.nodes[f->src <= PHOTON_MAX_NODE_ID ? f->src : 0];
    slot->last_seen_us = time_us_32();
    slot->consecutive_timeouts = 0;
    uint8_t count = f->len >= 2 ? f->payload[0] : 0;
    if (count > PHOTON_BATCH_MAX_EVENTS) {
        count = PHOTON_BATCH_MAX_EVENTS;
    }
    for (uint8_t i = 0; i < count; i++) {
        photon_event_t ev;
        memcpy(&ev, &f->payload[2 + i * sizeof ev], sizeof ev);
        if (slot->have_seq && (int16_t)(ev.seq - slot->next_evt_seq) < 0) {
            slot->dup_events++;
            continue;  // retransmitted duplicate
        }
        slot->have_seq = true;
        slot->next_evt_seq = (uint16_t)(ev.seq + 1);
        slot->events_rx++;
        if (P.sink) {
            P.sink(f->src, &ev);
        }
    }
}

static void bridge_send_poll(uint8_t node_id) {
    P.poll_seq++;
    photon_frame_t f = { 0 };
    f.type = PHOTON_FT_EVT_POLL;
    f.dst = node_id;
    f.seq = P.poll_seq;
    f.flags = PHOTON_FLAG_PRIO_EVENT;
    transport_send(&f, true);
    P.state = B_POLL_WAIT;
    P.wait_target = node_id;
    P.wait_deadline = make_timeout_time_us(PHOTON_POLL_TIMEOUT_US);
    P.nodes[node_id].polls++;
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
    if (P.bulk_pending) {
        return false;
    }
    memset(&P.bulk_req, 0, sizeof P.bulk_req);
    P.bulk_req.type = type;
    P.bulk_req.dst = dst;
    P.bulk_req.len = len;
    if (len > 0) {
        memcpy(P.bulk_req.payload, payload, len);
    }
    P.bulk_pending = true;
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

static void bridge_task(void) {
    switch (P.state) {
        case B_IDLE: {
            uint8_t target = bridge_next_alive(P.next_poll_id);
            if (target != 0) {
                // Interleave: one bulk slot after each full poll cycle.
                if (P.bulk_pending && target < P.next_poll_id) {
                    P.poll_seq++;
                    P.bulk_req.seq = P.poll_seq;
                    if (P.bulk_req.dst == PHOTON_ADDR_BROADCAST) {
                        transport_send(&P.bulk_req, false);
                        P.bulk_pending = false;  // no reply expected
                    } else {
                        transport_send(&P.bulk_req, false);
                        P.state = B_BULK_WAIT;
                        P.wait_target = P.bulk_req.dst;
                        P.wait_type = expected_reply_type(P.bulk_req.type);
                        P.wait_deadline = make_timeout_time_us(2 * PHOTON_POLL_TIMEOUT_US);
                        P.bulk_pending = false;
                        return;
                    }
                }
                P.next_poll_id = (uint8_t)((target % PHOTON_MAX_NODE_ID) + 1);
                P.retry_count = 0;
                bridge_send_poll(target);
                if (target >= bridge_next_alive(P.next_poll_id)) {
                    P.poll_cycles++;
                }
                return;
            }
            // No alive nodes: run discovery pings on cadence.
            if (time_reached(P.next_ping_at)) {
                P.next_ping_at = make_timeout_time_us(PHOTON_PING_INTERVAL_MS * 1000);
                photon_frame_t f = { 0 };
                f.type = PHOTON_FT_PING;
                f.dst = P.next_ping_id;
                P.poll_seq++;
                f.seq = P.poll_seq;
                transport_send(&f, true);
                P.state = B_BULK_WAIT;
                P.wait_target = P.next_ping_id;
                P.wait_type = PHOTON_FT_PONG;
                P.wait_deadline = make_timeout_time_us(PHOTON_POLL_TIMEOUT_US);
                P.next_ping_id = (uint8_t)((P.next_ping_id % PHOTON_MAX_NODE_ID) + 1);
            }
            break;
        }
        case B_POLL_WAIT:
            if (time_reached(P.wait_deadline)) {
                photon_node_slot_t *slot = &P.nodes[P.wait_target];
                if (P.retry_count < PHOTON_POLL_RETRIES) {
                    P.retry_count++;
                    slot->retries++;
                    // Same poll seq: node resends the identical batch.
                    photon_frame_t f = { 0 };
                    f.type = PHOTON_FT_EVT_POLL;
                    f.dst = P.wait_target;
                    f.seq = P.poll_seq;
                    f.flags = PHOTON_FLAG_PRIO_EVENT;
                    transport_send(&f, true);
                    P.wait_deadline = make_timeout_time_us(PHOTON_POLL_TIMEOUT_US);
                } else {
                    slot->timeouts++;
                    if (++slot->consecutive_timeouts >= 8) {
                        slot->alive = false;
                        log_note("node %u silent, dropped from poll cycle", P.wait_target);
                    }
                    P.state = B_IDLE;
                }
            }
            break;
        case B_BULK_WAIT:
            if (time_reached(P.wait_deadline)) {
                P.state = B_IDLE;  // console request timed out; console reports staleness
            }
            break;
    }
}

// ---------------------------------------------------------------------------

void protocol_on_frame(const photon_frame_t *f) {
    if (!P.is_bridge) {
        if (f->type == PHOTON_FT_EVT_POLL) {
            node_handle_evt_poll(f);
        } else {
            node_handle_request(f);
        }
        return;
    }

    // Bridge side.
    if (f->src > PHOTON_MAX_NODE_ID) {
        return;
    }
    photon_node_slot_t *slot = &P.nodes[f->src];
    slot->last_seen_us = time_us_32();

    switch (f->type) {
        case PHOTON_FT_EVT_BATCH:
            if (P.state == B_POLL_WAIT && f->src == P.wait_target) {
                P.state = B_IDLE;
            }
            bridge_handle_batch(f);
            break;
        case PHOTON_FT_PONG:
            if (!slot->alive) {
                slot->alive = true;
                slot->have_seq = false;
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
            console_on_bridge_response(f);
            break;
    }
}

void protocol_task(void) {
    if (P.is_bridge) {
        bridge_task();
    }
}
