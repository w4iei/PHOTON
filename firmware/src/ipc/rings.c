#include "ipc/rings.h"

#include "pico/time.h"

photon_event_ring_t g_event_ring;
photon_snapshot_t g_snapshot;
photon_cmd_mailbox_t g_cmd_mailbox;
photon_trace_ring_t g_trace_ring;

void rings_init(void) {
    memset(&g_event_ring, 0, sizeof g_event_ring);
    memset(&g_snapshot, 0, sizeof g_snapshot);
    memset(&g_cmd_mailbox, 0, sizeof g_cmd_mailbox);
    memset(&g_trace_ring, 0, sizeof g_trace_ring);
}

void snapshot_read(photon_snapshot_t *out) {
    const photon_snapshot_t *s = &g_snapshot;
    uint32_t s1, s2;
    do {
        s1 = s->seq;
        if (s1 & 1u) {
            continue;
        }
        __dmb();
        memcpy(out->value, (const void *)s->value, sizeof out->value);
        memcpy(out->min, (const void *)s->min, sizeof out->min);
        memcpy(out->max, (const void *)s->max, sizeof out->max);
        out->sweep_count = s->sweep_count;
        out->sweep_us = s->sweep_us;
        __dmb();
        s2 = s->seq;
    } while (s1 != s2 || (s1 & 1u));
    out->seq = s2;
}

bool cmd_mailbox_push(const photon_cmd_t *cmd) {
    photon_cmd_mailbox_t *m = &g_cmd_mailbox;
    if (m->tail - m->head >= PHOTON_CMD_MAILBOX_SLOTS) {
        m->drops++;
        return false;
    }
    m->slots[m->tail % PHOTON_CMD_MAILBOX_SLOTS] = *cmd;
    __dmb();
    m->tail = m->tail + 1;
    return true;
}

bool cmd_mailbox_park_request(uint32_t timeout_us) {
    photon_cmd_mailbox_t *m = &g_cmd_mailbox;
    photon_cmd_t park = { .op = PHOTON_CMD_PARK };
    m->park_requested = true;
    __dmb();
    if (!cmd_mailbox_push(&park)) {
        m->park_requested = false;
        return false;  // mailbox full: caller logs and proceeds unparked
    }
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (!m->parked) {
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) {
            m->park_requested = false;
            return false;
        }
        __dmb();
    }
    return true;
}

void cmd_mailbox_park_release(void) {
    g_cmd_mailbox.park_requested = false;
    __dmb();
}
