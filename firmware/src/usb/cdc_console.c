#include "usb/cdc_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/time.h"
#include "tusb.h"

#include "board_config.h"
#include "bridge/midi_map.h"
#include "comms/protocol.h"
#include "comms/transport.h"
#include "config/config_store.h"
#include "core1/events.h"
#include "core1/scan.h"
#include "ipc/rings.h"
#include "util/log.h"

static struct {
    bool is_bridge;
    bool sensor_role;
    char line[128];
    size_t line_len;
    // trace session
    bool trace_active;
    bool trace_started;   // BEGIN_TRACE emitted
    bool trace_remote;
    uint8_t trace_node;
    uint8_t trace_sensor;
    uint32_t trace_t0_us;
    absolute_time_t next_trace_pull;
    // duration-bound capture (host tool contract: "capture <seconds>")
    bool capture_timed;
    absolute_time_t capture_deadline;
} C;

void console_init(bool is_bridge, bool sensor_role) {
    memset(&C, 0, sizeof C);
    C.is_bridge = is_bridge;
    C.sensor_role = sensor_role;
    C.trace_sensor = PHOTON_TRACE_DEFAULT_SENSOR;
}

// Core-1 commands must not fail silently: a dropped mode/cal/trace command
// would leave runtime state diverged from what the operator believes.
static void push_core1_cmd(const photon_cmd_t *cmd) {
    if (!cmd_mailbox_push(cmd)) {
        log_note("core-1 mailbox full — command dropped, retry");
    }
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

static void print_help(void) {
    log_printf("PHOTON native console. Commands:");
    log_printf("  stats            local + node counters");
    log_printf("  nodes            bridge poll table");
    log_printf("  data [id]        latest sweep values");
    log_printf("  minmax [id]      calibration min/max");
    log_printf("  ping <id>        probe a node id");
    log_printf("  trace <sensor> [node] / trace stop");
    log_printf("  capture [sec]    timed local trace of last/default sensor");
    log_printf("  burst <n> [id]   inject synthetic events (torture test)");
    log_printf("  cal save|reset   persist / clear calibration");
    log_printf("  mode <0|1|2>     scan: 0=seq 1=parallel 2=two-phase");
    log_printf("  disable|enable <idx>  mask a local sensor (persist: cal save)");
    log_printf("  setid <n>        set this node's bus id (1-%d)", PHOTON_MAX_NODE_ID);
    log_printf("  flashtest        hammer flash while core 1 scans (M1 proof)");
    log_printf("  reboot / bootsel restart firmware / enter UF2 bootloader");
    log_printf("  id               role/version summary");
}

static void print_values_row(const char *label, const uint16_t *v, int n) {
    char buf[220];
    int off = snprintf(buf, sizeof buf, "%s", label);
    for (int i = 0; i < n && off < (int)sizeof buf - 8; i++) {
        off += snprintf(buf + off, sizeof buf - (size_t)off, " %5u", v[i]);
    }
    log_printf("%s", buf);
}

static void print_local_stats(void) {
    const transport_stats_t *ts = transport_stats();
    const frame_parse_stats_t *ps = transport_parse_stats();
    log_printf("[STAT] role=%s addr=%u cfg_v%lu%s",
               C.is_bridge ? "bridge" : "node",
               C.is_bridge ? 0 : g_config.node_id,
               (unsigned long)g_config.version,
               g_config_from_flash ? "" : " (defaults, uncalibrated)");
    if (C.sensor_role) {
        uint32_t hz10 = g_scan_ctl.sweep_us ? 10000000u / g_scan_ctl.sweep_us : 0;
        log_printf("[STAT] sweeps=%lu sweep_us=%lu (%lu.%lu Hz) mode=%u reinit=%lu "
                   "evt_on=%lu evt_off=%lu ring_ovf=%lu",
                   (unsigned long)g_scan_ctl.sweep_count,
                   (unsigned long)g_scan_ctl.sweep_us,
                   (unsigned long)(hz10 / 10), (unsigned long)(hz10 % 10),
                   (unsigned)g_scan_ctl.mode,
                   (unsigned long)g_scan_ctl.reinit_count,
                   (unsigned long)g_events.events_on,
                   (unsigned long)g_events.events_off,
                   (unsigned long)g_event_ring.overflows);
    }
    log_printf("[STAT] bus tx=%lu rx=%lu crc_err=%lu hdr_err=%lu cmd_drops=%lu",
               (unsigned long)ts->tx_frames, (unsigned long)ts->rx_frames,
               (unsigned long)ps->crc_errors, (unsigned long)ps->hdr_errors,
               (unsigned long)g_cmd_mailbox.drops);
    if (C.is_bridge) {
        log_printf("[STAT] poll_cycles=%lu midi_on=%lu midi_off=%lu",
                   (unsigned long)protocol_poll_cycles(),
                   (unsigned long)midi_map_notes_on_sent(),
                   (unsigned long)midi_map_notes_off_sent());
    }
}

static void print_nodes(void) {
    const photon_node_slot_t *t = protocol_node_table();
    for (int id = 1; id <= PHOTON_MAX_NODE_ID; id++) {
        const photon_node_slot_t *s = &t[id];
        if (!s->alive && s->polls == 0) {
            continue;
        }
        log_printf("node %d: %s polls=%lu events=%lu dup=%lu gaps=%lu malformed=%lu "
                   "retries=%lu timeouts=%lu",
                   id, s->alive ? "alive" : "SILENT",
                   (unsigned long)s->polls, (unsigned long)s->events_rx,
                   (unsigned long)s->dup_events, (unsigned long)s->seq_gaps,
                   (unsigned long)s->malformed, (unsigned long)s->retries,
                   (unsigned long)s->timeouts);
    }
}

static void trace_begin_header(void) {
    // Exact contract expected by host_code/listen_for_single_sensor_high_res.py
    log_printf("BEGIN_TRACE sensor=%u midi=None note=None polarity=NOR thr_on=0 thr_off=0",
               C.trace_sensor);
    C.trace_started = true;
    C.trace_t0_us = 0;
}

// Sample lines are written unflushed (callers flush once per batch) so a
// 1-2 kHz trace doesn't force one USB packet per 15-byte line.
static void trace_emit_sample(uint32_t t_us, uint16_t value) {
    if (!C.trace_started) {
        trace_begin_header();
    }
    if (C.trace_t0_us == 0) {
        C.trace_t0_us = t_us ? t_us : 1;
    }
    uint32_t rel = t_us - C.trace_t0_us;
    if (!tud_cdc_connected()) {
        return;
    }
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lu.%06lu,%u\r\n",
                     (unsigned long)(rel / 1000000u),
                     (unsigned long)(rel % 1000000u), value);
    tud_cdc_write(buf, (uint32_t)n);
}

static void trace_stop(void) {
    if (C.trace_active) {
        if (C.trace_remote) {
            uint8_t p[2] = { C.trace_sensor, 0 };
            if (!protocol_bridge_request(PHOTON_FT_TRACE_START, C.trace_node, p, 2)) {
                log_note("trace stop: request queue full, node keeps tapping");
            }
        } else if (C.sensor_role) {
            photon_cmd_t cmd = { .op = PHOTON_CMD_TRACE_TAP, .arg8 = C.trace_sensor, .a = 0 };
            push_core1_cmd(&cmd);
        }
        if (C.trace_started) {
            log_printf("END_TRACE");
        }
    }
    C.trace_active = false;
    C.trace_started = false;
    C.capture_timed = false;
}

// Duration-bound local trace: the wire contract of the legacy trace mode
// that software/host_code/listen_for_single_sensor_high_res.py triggers
// with "capture <seconds>".
static void capture_start(float seconds) {
    if (!C.sensor_role) {
        log_note("capture: no sensors on this board (use trace <s> <node> on the bridge)");
        return;
    }
    trace_stop();
    C.trace_active = true;
    C.trace_remote = false;
    C.trace_started = false;
    C.capture_timed = true;
    if (seconds <= 0.0f || seconds > 600.0f) {
        seconds = (float)PHOTON_CAPTURE_DEFAULT_S;
    }
    C.capture_deadline = make_timeout_time_us((uint64_t)(seconds * 1e6f));
    photon_cmd_t cmd = { .op = PHOTON_CMD_TRACE_TAP, .arg8 = C.trace_sensor, .a = 1 };
    push_core1_cmd(&cmd);
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

static void handle_line(char *line) {
    char *save = NULL;
    char *cmd = strtok_r(line, " \t", &save);
    if (cmd == NULL) {
        return;
    }
    char *a1 = strtok_r(NULL, " \t", &save);
    char *a2 = strtok_r(NULL, " \t", &save);

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_help();
    } else if (strcmp(cmd, "id") == 0) {
        log_printf("PHOTON native fw | role=%s | addr=%u | banks=%s | cfg v%lu",
                   C.is_bridge ? "bridge" : "sensor-node",
                   C.is_bridge ? 0 : g_config.node_id,
                   C.sensor_role ? "present" : "none",
                   (unsigned long)g_config.version);
    } else if (strcmp(cmd, "stats") == 0) {
        print_local_stats();
        if (C.is_bridge && a1 != NULL) {
            protocol_bridge_request(PHOTON_FT_STATS_REQ, (uint8_t)atoi(a1), NULL, 0);
        }
    } else if (strcmp(cmd, "nodes") == 0) {
        print_nodes();
    } else if (strcmp(cmd, "data") == 0) {
        if (C.sensor_role) {
            photon_snapshot_t snap;
            snapshot_read(&snap);
            print_values_row("[DATA]", snap.value, PHOTON_ACTIVE_SENSORS);
        } else if (C.is_bridge) {
            protocol_bridge_request(PHOTON_FT_DATA_REQ, a1 ? (uint8_t)atoi(a1) : 1, NULL, 0);
        }
    } else if (strcmp(cmd, "minmax") == 0) {
        if (C.sensor_role) {
            photon_snapshot_t snap;
            snapshot_read(&snap);
            print_values_row("[MIN] ", snap.min, PHOTON_ACTIVE_SENSORS);
            print_values_row("[MAX] ", snap.max, PHOTON_ACTIVE_SENSORS);
        } else if (C.is_bridge) {
            uint8_t p[2] = { 0, PHOTON_ACTIVE_SENSORS };
            protocol_bridge_request(PHOTON_FT_MINMAX_REQ, a1 ? (uint8_t)atoi(a1) : 1, p, 2);
        }
    } else if (strcmp(cmd, "ping") == 0 && a1 != NULL) {
        if (C.is_bridge) {
            protocol_bridge_request(PHOTON_FT_PING, (uint8_t)atoi(a1), NULL, 0);
        }
    } else if (strcmp(cmd, "capture") == 0) {
        capture_start(a1 ? (float)atof(a1) : (float)PHOTON_CAPTURE_DEFAULT_S);
    } else if (strcmp(cmd, "trace") == 0) {
        if (a1 != NULL && strcmp(a1, "stop") == 0) {
            trace_stop();
        } else if (a1 != NULL) {
            trace_stop();
            C.trace_sensor = (uint8_t)atoi(a1);
            C.trace_active = true;
            C.trace_started = false;
            if (C.is_bridge && !C.sensor_role) {
                C.trace_remote = true;
                C.trace_node = a2 ? (uint8_t)atoi(a2) : 1;
                uint8_t p[2] = { C.trace_sensor, 1 };
                protocol_bridge_request(PHOTON_FT_TRACE_START, C.trace_node, p, 2);
                C.next_trace_pull = make_timeout_time_us(50000);
            } else if (C.sensor_role) {
                C.trace_remote = false;
                photon_cmd_t cmdm = { .op = PHOTON_CMD_TRACE_TAP,
                                      .arg8 = C.trace_sensor, .a = 1 };
                push_core1_cmd(&cmdm);
            }
        }
    } else if (strcmp(cmd, "burst") == 0 && a1 != NULL) {
        uint16_t n = (uint16_t)atoi(a1);
        if (C.is_bridge) {
            uint8_t p[2];
            memcpy(p, &n, 2);
            uint8_t dst = a2 ? (uint8_t)atoi(a2) : PHOTON_ADDR_BROADCAST;
            protocol_bridge_request(PHOTON_FT_TEST_BURST, dst, p, 2);
            log_info("burst %u -> %u queued", n, dst);
        } else if (C.sensor_role) {
            photon_cmd_t cmdm = { .op = PHOTON_CMD_TEST_BURST, .a = n };
            push_core1_cmd(&cmdm);
        }
    } else if (strcmp(cmd, "cal") == 0 && a1 != NULL) {
        if (strcmp(a1, "save") == 0) {
            if (C.is_bridge) {
                const photon_node_slot_t *t = protocol_node_table();
                int queued = 0, dropped = 0;
                for (int id = 1; id <= PHOTON_MAX_NODE_ID; id++) {
                    if (t[id].alive) {
                        if (protocol_bridge_request(PHOTON_FT_CAL_COMMIT, (uint8_t)id, NULL, 0)) {
                            queued++;
                        } else {
                            dropped++;
                        }
                    }
                }
                log_info("CAL_COMMIT queued to %d node(s)%s", queued,
                         dropped ? " — QUEUE FULL, retry cal save" : "");
            } else {
                config_store_save();
            }
        } else if (strcmp(a1, "reset") == 0) {
            if (C.is_bridge) {
                uint8_t p[5] = { PHOTON_CAL_IDX_ALL, 0, 0, 0, 0 };
                protocol_bridge_request(PHOTON_FT_CAL_SET, PHOTON_ADDR_BROADCAST, p, 5);
            } else if (C.sensor_role) {
                photon_cmd_t cmdm = { .op = PHOTON_CMD_RESET_CAL };
                push_core1_cmd(&cmdm);
            }
            log_info("calibration reset");
        }
    } else if ((strcmp(cmd, "disable") == 0 || strcmp(cmd, "enable") == 0) && a1 != NULL) {
        int idx = atoi(a1);
        if (!C.sensor_role) {
            log_note("%s: no sensors on this board", cmd);
        } else if (idx >= 0 && idx < PHOTON_MAX_SENSORS) {
            if (cmd[0] == 'd') {
                g_config.local_disabled_mask |= 1u << idx;
            } else {
                g_config.local_disabled_mask &= ~(1u << idx);
            }
            photon_cmd_t cmdm = { .op = PHOTON_CMD_SET_DISABLED,
                                  .a = g_config.local_disabled_mask };
            push_core1_cmd(&cmdm);
            log_info("sensor %d %sd (persist with 'cal save')", idx, cmd);
        }
    } else if (strcmp(cmd, "mode") == 0 && a1 != NULL) {
        uint8_t m = (uint8_t)atoi(a1);
        if (C.sensor_role && m <= PHOTON_SCAN_TWO_PHASE) {
            photon_cmd_t cmdm = { .op = PHOTON_CMD_SCAN_MODE, .arg8 = m };
            push_core1_cmd(&cmdm);
            g_config.scan_mode = m;
            log_info("scan mode -> %u", m);
        }
    } else if (strcmp(cmd, "setid") == 0 && a1 != NULL) {
        int id = atoi(a1);
        if (id >= 1 && id <= PHOTON_MAX_NODE_ID) {
            g_config.node_id = (uint8_t)id;
            config_store_save();
            log_info("node id -> %d (takes effect on reboot)", id);
        }
    } else if (strcmp(cmd, "reboot") == 0) {
        // SWD-free maintenance: restart the firmware over USB alone.
        log_printf("rebooting...");
        tud_cdc_write_flush();
        sleep_ms(50);
        watchdog_reboot(0, 0, 100);
    } else if (strcmp(cmd, "bootsel") == 0) {
        // Enter the UF2 bootloader without touching the USB-BOOT button.
        log_printf("entering BOOTSEL — copy photon.uf2 or use picotool load");
        tud_cdc_write_flush();
        sleep_ms(50);
        reset_usb_boot(0, 0);
    } else if (strcmp(cmd, "flashtest") == 0) {
        uint32_t before_us = g_scan_ctl.sweep_us;
        uint32_t before_count = g_scan_ctl.sweep_count;
        int n = config_store_flashtest(10);
        log_info("flashtest: %d erase+program cycles done", n);
        log_info("sweep_us before=%lu after=%lu sweeps_during=%lu (expect steady rate)",
                 (unsigned long)before_us, (unsigned long)g_scan_ctl.sweep_us,
                 (unsigned long)(g_scan_ctl.sweep_count - before_count));
    } else {
        log_printf("unknown command '%s' (try 'help')", cmd);
    }
}

// ---------------------------------------------------------------------------
// Bridge response pretty-printing
// ---------------------------------------------------------------------------

void console_on_bridge_response(const photon_frame_t *f) {
    switch (f->type) {
        case PHOTON_FT_DATA_RESP: {
            // Never trust wire-supplied counts past local buffers: clamp to
            // both the frame length and the destination array size.
            uint8_t n = f->len >= 1 ? f->payload[0] : 0;
            if (n > (f->len - 1) / 2) {
                n = (uint8_t)((f->len - 1) / 2);
            }
            if (n > PHOTON_MAX_SENSORS) {
                n = PHOTON_MAX_SENSORS;
            }
            uint16_t vals[PHOTON_MAX_SENSORS];
            memcpy(vals, &f->payload[1], (size_t)n * 2);
            char label[24];
            snprintf(label, sizeof label, "[DATA %u]", f->src);
            print_values_row(label, vals, n);
            break;
        }
        case PHOTON_FT_MINMAX_RESP: {
            uint8_t count = f->len >= 2 ? f->payload[1] : 0;
            if (count > (f->len >= 2 ? (uint8_t)((f->len - 2) / 4) : 0)) {
                count = f->len >= 2 ? (uint8_t)((f->len - 2) / 4) : 0;
            }
            if (count > PHOTON_MAX_SENSORS) {
                count = PHOTON_MAX_SENSORS;
            }
            uint16_t mn[PHOTON_MAX_SENSORS], mx[PHOTON_MAX_SENSORS];
            for (uint8_t i = 0; i < count; i++) {
                memcpy(&mn[i], &f->payload[2 + i * 4], 2);
                memcpy(&mx[i], &f->payload[4 + i * 4], 2);
            }
            char label[24];
            snprintf(label, sizeof label, "[MIN %u] ", f->src);
            print_values_row(label, mn, count);
            snprintf(label, sizeof label, "[MAX %u] ", f->src);
            print_values_row(label, mx, count);
            break;
        }
        case PHOTON_FT_STATS_RESP: {
            if (f->len >= sizeof(photon_stats_payload_t)) {
                photon_stats_payload_t st;
                memcpy(&st, f->payload, sizeof st);
                uint32_t hz10 = st.sweep_us ? 10000000u / st.sweep_us : 0;
                log_printf("[STAT node %u] sweep_us=%u (%lu.%lu Hz) sweeps=%lu mode=%u "
                           "evt_on=%lu evt_off=%lu ovf=%lu crc=%lu hdr=%lu reinit=%u "
                           "trace_drop=%u",
                           f->src, st.sweep_us,
                           (unsigned long)(hz10 / 10), (unsigned long)(hz10 % 10),
                           (unsigned long)st.sweep_count, st.scan_mode,
                           (unsigned long)st.events_on, (unsigned long)st.events_off,
                           (unsigned long)st.ring_overflows, (unsigned long)st.crc_errors,
                           (unsigned long)st.hdr_errors, st.reinit_count,
                           st.trace_dropped);
            }
            break;
        }
        case PHOTON_FT_TRACE_DATA: {
            if (!C.trace_active || !C.trace_remote) {
                break;
            }
            uint8_t n = f->len >= 2 ? f->payload[1] : 0;
            uint8_t max_n = f->len >= 2 ? (uint8_t)((f->len - 2) / 6) : 0;
            if (n > max_n) {
                n = max_n;  // wire count must fit the actual frame
            }
            for (uint8_t i = 0; i < n; i++) {
                uint32_t t_us;
                uint16_t v;
                memcpy(&t_us, &f->payload[2 + i * 6], 4);
                memcpy(&v, &f->payload[6 + i * 6], 2);
                trace_emit_sample(t_us, v);
            }
            tud_cdc_write_flush();
            break;
        }
        case PHOTON_FT_CAL_ACK:
            log_info("ack from node %u: %s", f->src,
                     f->len >= 1 && f->payload[0] ? "ok" : "FAIL");
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------

void console_task(void) {
    // Input.
    while (tud_cdc_available()) {
        uint8_t ch;
        if (tud_cdc_read(&ch, 1) != 1) {
            break;
        }
        if (ch == '\r' || ch == '\n') {
            if (C.line_len > 0) {
                C.line[C.line_len] = 0;
                C.line_len = 0;
                log_printf("> %s", C.line);
                handle_line(C.line);
            }
        } else if (ch == 0x7F || ch == '\b') {
            if (C.line_len > 0) {
                C.line_len--;
            }
        } else if (C.line_len < sizeof C.line - 1) {
            C.line[C.line_len++] = (char)ch;
        }
    }

    // Local trace streaming (sensor role); one flush per batch.
    if (C.trace_active && !C.trace_remote && C.sensor_role) {
        photon_trace_sample_t s;
        int budget = 32;  // bound CDC time per loop
        bool wrote = false;
        while (budget-- > 0 && trace_ring_pop(&s)) {
            trace_emit_sample(s.t_us, s.value);
            wrote = true;
        }
        if (wrote) {
            tud_cdc_write_flush();
        }
        if (C.capture_timed && time_reached(C.capture_deadline)) {
            trace_stop();  // emits END_TRACE for the host capture tool
        }
    }

    // Remote trace pulls (bridge role), throttled.
    if (C.trace_active && C.trace_remote && time_reached(C.next_trace_pull)) {
        if (protocol_bridge_request(PHOTON_FT_TRACE_DATA, C.trace_node, NULL, 0)) {
            C.next_trace_pull = make_timeout_time_us(20000);  // 50 Hz x 21 samples > 1 kHz
        }
    }
}
