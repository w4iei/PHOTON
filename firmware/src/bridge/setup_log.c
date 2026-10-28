#include "bridge/setup_log.h"

#include <string.h>

#include "pico/time.h"
#include "pico/unique_id.h"

#include "board_config.h"
#include "bridge/midi_map.h"
#include "bridge/recorder.h"
#include "bridge/setup_text.h"
#include "cal/cal_session.h"
#include "comms/protocol.h"
#include "config/config_store.h"

#include "build_id.h"  // generated every build: PHOTON_BUILD_ID, PHOTON_BUILD_DATE

// Discovery pings one silent id every PHOTON_PING_INTERVAL_MS, so every
// board powered with the bridge is in the poll cycle well before this.
#define SETTLE_MS  3000
#define REPLY_MS   600    // per board, from its last page request
#define PAGES      ((PHOTON_ACTIVE_SENSORS + CAL_INFO_PER_FRAME - 1) / CAL_INFO_PER_FRAME)
#define ALL_PAGES  ((uint8_t)((1u << PAGES) - 1u))

static struct {
    bool enabled;
    bool done;
    absolute_time_t settle_at;
    uint8_t id;          // board being read, 0 = none yet
    uint8_t asked;       // its pages requested (bit per page)
    uint8_t got;         // its pages received
    absolute_time_t deadline;
} L;
static setup_info_t info;
static char text[SETUP_TEXT_MAX];
static char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

void setup_log_init(void) {
    memset(&L, 0, sizeof L);
    memset(&info, 0, sizeof info);  // every board SETUP_NODE_ABSENT
    L.enabled = true;
    L.settle_at = make_timeout_time_ms(SETTLE_MS);
}

static void finish(void) {
    pico_get_unique_board_id_string(serial, sizeof serial);
    info.build_id = PHOTON_BUILD_ID;
    info.build_date = PHOTON_BUILD_DATE;
    info.serial = serial;
    info.read_ms = to_ms_since_boot(get_absolute_time());
    for (uint32_t m = 0; m < PHOTON_MAX_MANUALS; m++) {
        info.channel[m] = (uint8_t)(midi_map_channel_for_manual(m) + 1);
        info.channel_auto[m] = g_config.manual_channel[m] == 0;
    }
    info.midi_low = g_config.midi_low;
    info.midi_high = g_config.midi_high;
    info.vel_out_min = g_config.vel_out_min;
    info.vel_out_max = g_config.vel_out_max;
    info.vel_min_ms = g_config.vel_min_ms;
    info.vel_max_ms = g_config.vel_max_ms;
    info.vel_curve = g_config.vel_curve;
    info.strike_global = cal_session_global();
    info.rules = cal_session_rules(&info.margin_pct);
    size_t n = setup_text_format(text, sizeof text, &info);
    recorder_set_setup(text, (uint32_t)n);
    L.done = true;
}

// Next board on the bus after the one just read; the text once none is left.
static void next_board(void) {
    const photon_node_slot_t *t = protocol_node_table();
    for (uint8_t id = (uint8_t)(L.id + 1); id <= PHOTON_MAX_NODE_ID; id++) {
        if (t[id].alive) {
            L.id = id;
            L.asked = 0;
            L.got = 0;
            L.deadline = make_timeout_time_ms(REPLY_MS);
            for (uint8_t i = 0; i < PHOTON_ACTIVE_SENSORS; i++) {
                info.node[id].note[i] = midi_map_note(id, i);
            }
            return;
        }
    }
    finish();
}

void setup_log_task(void) {
    if (!L.enabled || L.done || !time_reached(L.settle_at)) {
        return;
    }
    if (L.id == 0) {
        next_board();
        return;
    }
    // Pages go out as the bulk queue has room (the console shares it).
    for (uint8_t page = 0; page < PAGES; page++) {
        if (L.asked & (1u << page)) {
            continue;
        }
        uint8_t p[2] = { (uint8_t)(page * CAL_INFO_PER_FRAME), CAL_INFO_PER_FRAME };
        if (!protocol_bridge_request(PHOTON_FT_CAL_INFO_REQ, L.id, p, 2)) {
            break;
        }
        L.asked |= (uint8_t)(1u << page);
        L.deadline = make_timeout_time_ms(REPLY_MS);
    }
    if (L.got == ALL_PAGES) {
        info.node[L.id].state = SETUP_NODE_READ;
    } else if (time_reached(L.deadline)) {
        info.node[L.id].state = SETUP_NODE_NO_REPLY;
    } else {
        return;
    }
    next_board();
}

bool setup_log_on_response(const photon_frame_t *f) {
    if (!L.enabled || L.done || L.id == 0 || f->type != PHOTON_FT_CAL_INFO_RESP ||
        f->src != L.id || f->len < 3) {
        return false;
    }
    uint8_t start = f->payload[0];
    uint8_t page = (uint8_t)(start / CAL_INFO_PER_FRAME);
    if (start % CAL_INFO_PER_FRAME || page >= PAGES || !(L.asked & (1u << page)) ||
        (L.got & (1u << page))) {
        return false;  // not one of ours: 'cal compare' prints it
    }
    uint8_t count = f->payload[1];
    uint8_t fit = (uint8_t)((f->len - 3) / sizeof(cal_info_rec_t));
    if (count > fit) {
        count = fit;
    }
    if (count > PHOTON_ACTIVE_SENSORS - start) {
        count = (uint8_t)(PHOTON_ACTIVE_SENSORS - start);
    }
    setup_node_t *nd = &info.node[L.id];
    nd->flags = f->payload[2];
    memcpy(&nd->key[start], &f->payload[3], (size_t)count * sizeof(cal_info_rec_t));
    L.got |= (uint8_t)(1u << page);
    return true;
}
