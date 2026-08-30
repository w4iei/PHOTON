#include "usb/cdc_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/time.h"
#include "pico/unique_id.h"
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
    // legacy-style verbosity: live event lines + 5 s heartbeat ('log on|off')
    bool log_events;
    absolute_time_t next_heartbeat_at;
} C;

void console_init(bool is_bridge, bool sensor_role) {
    memset(&C, 0, sizeof C);
    C.is_bridge = is_bridge;
    C.sensor_role = sensor_role;
    C.trace_sensor = PHOTON_TRACE_DEFAULT_SENSOR;
    C.log_events = true;
    C.next_heartbeat_at = make_timeout_time_ms(5000);
}

static const char *note_name(int16_t note, char *buf, size_t n) {
    static const char *names[12] = { "C", "C#", "D", "D#", "E", "F",
                                     "F#", "G", "G#", "A", "A#", "B" };
    snprintf(buf, n, "%s%d", names[note % 12], note / 12 - 1);
    return buf;
}

void console_print_event(uint8_t node_id, const photon_event_t *ev,
                         int16_t note, uint8_t velocity, uint8_t channel) {
    if (!C.log_events) {
        return;
    }
    char nm[8];
    log_printf("[EVT] %-3s node=%u s=%-2u %s(%d) ch=%u vel=%-3u dt=%lu.%lums",
               ev->state ? "ON" : "OFF", node_id, ev->local_idx,
               note_name(note, nm, sizeof nm), note, channel + 1, velocity,
               (unsigned long)(ev->dt_us / 1000u),
               (unsigned long)((ev->dt_us % 1000u) / 100u));
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

// NODECTL payload builder (bridge role): op u8 | arg u16 (LE).
static void send_nodectl(uint8_t op, uint16_t arg, uint8_t dst) {
    uint8_t p[3];
    p[0] = op;
    memcpy(&p[1], &arg, 2);
    protocol_bridge_request(PHOTON_FT_NODECTL, dst, p, 3);
}

static void print_help(void) {
    log_printf("PHOTON native console. Commands:");
    log_printf("  stats            local + node counters");
    log_printf("  nodes            bridge poll table");
    log_printf("  data [id]        latest sweep values");
    log_printf("  minmax [id]      calibration min/max");
    log_printf("  ping <id>        probe a node id");
    log_printf("  <Enter>          live sensor table");
    log_printf("  r / s / x        calibrate: start / freeze+save / abort");
    log_printf("  trace <sensor> [node] / trace stop");
    log_printf("  capture [sec]    timed local trace of last/default sensor");
    log_printf("  burst <n> [id]   inject one-shot synthetic events");
    log_printf("  test <evps|stop> [id]  pseudorandom load for loss validation");
    log_printf("  cal save|reset [id]  persist / clear calibration (one node or all)");
    log_printf("  mode <0|1|2> [id]    scan: 0=seq 1=parallel 2=two-phase (bridge: remote)");
    log_printf("  rate <hz>|max [id]   paced sweep rate (saved; bridge: remote)");
    log_printf("  settle <us> [id]     emitter settle for A/B (5-500; NOT saved)");
    log_printf("  localmidi on|off node plays its own USB-MIDI (default off; saved)");
    log_printf("  velrange <min> <max> MIDI velocity output range (1-127; saved,"
               " broadcast)");
    log_printf("  disable|enable <idx>  mask a sensor (node: local idx; bridge: global idx)");
    log_printf("  setid <n> | setid <node> <newid>   bus id, local or remote");
    log_printf("  chmap [m] [ch|auto]  per-manual MIDI channel map (bridge; manual 1-%d,"
               " user channel 1-16)", PHOTON_MAX_MANUALS);
    log_printf("  flashtest        hammer flash while core 1 scans (M1 proof)");
    log_printf("  reboot|bootsel [id]  restart / UF2 bootloader (bridge: remote node)");
    log_printf("  id               role/version summary");
}

void console_print_banner(int banks_found) {
    // Printed on every console attach (legacy sensor-node boot banner, native
    // edition): who we are, where the project lives, and what you can type.
    char uid[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(uid, sizeof uid);
    log_printf(".*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*");
    log_printf(".*.*.*  PHOTON  --  native firmware  *.*.*");
    log_printf(".*.   https://github.com/w4iei/photon   .*");
    log_printf(".*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*.*");
    log_printf("Creator: Noah Jaffe");
    log_printf(" ");
    log_printf("[BOOT] role: %s%s | banks=%d | bus addr=%u",
               C.is_bridge ? "bridge/MIDI host" : "sensor node",
               C.sensor_role ? "" : " (no sensor array)", banks_found,
               C.is_bridge ? 0 : g_config.node_id);
    log_printf("[BOOT] hw id: %s | cfg v%lu%s", uid,
               (unsigned long)g_config.version,
               g_config_from_flash ? "" : " (defaults, uncalibrated)");
    log_printf("[CFG] RS-485: %.2f Mbaud | scan: %u Hz paced | vel: %u-%u",
               (double)PHOTON_RS485_BAUD / 1e6,
               g_config.scan_rate_hz ? g_config.scan_rate_hz
                                     : PHOTON_DEFAULT_SCAN_RATE_HZ,
               (unsigned)g_config.vel_out_min, (unsigned)g_config.vel_out_max);
    log_printf(" ");
    print_help();
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
        uint32_t hz10 = g_scan_ctl.period_us ? 10000000u / g_scan_ctl.period_us : 0;
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
    log_printf("[STAT] bus tx=%lu rx=%lu crc_err=%lu hdr_err=%lu cmd_drops=%lu log_drop=%lu",
               (unsigned long)ts->tx_frames, (unsigned long)ts->rx_frames,
               (unsigned long)ps->crc_errors, (unsigned long)ps->hdr_errors,
               (unsigned long)g_cmd_mailbox.drops,
               (unsigned long)log_dropped_bytes);
    if (C.is_bridge) {
        log_printf("[STAT] poll_cycles=%lu midi_on=%lu midi_off=%lu",
                   (unsigned long)protocol_poll_cycles(),
                   (unsigned long)midi_map_notes_on_sent(),
                   (unsigned long)midi_map_notes_off_sent());
    } else {
        // Sensor node: MIDI counters advance in standalone-over-USB mode.
        log_printf("[STAT] midi_on=%lu midi_off=%lu (standalone when unpolled)",
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

static unsigned isqrt32(uint32_t x) {
    unsigned r = 0;
    for (int s = 15; s >= 0; s--) {
        unsigned t = r | (1u << s);
        if ((uint32_t)t * t <= x) {
            r = t;
        }
    }
    return r;
}

// Legacy-style live table on a bare Enter.
static void print_table(void) {
    if (!C.sensor_role) {
        print_local_stats();
        print_nodes();
        return;
    }
    photon_snapshot_t snap;
    snapshot_read(&snap);
    uint32_t period = g_scan_ctl.period_us;
    uint32_t hz10 = period ? 10000000u / period : 0;
    log_printf("== PHOTON node %u | %lu.%lu Hz (scan %lu us) | cal %s | cfg v%lu | evt %lu/%lu ==",
               g_config.node_id,
               (unsigned long)(hz10 / 10), (unsigned long)(hz10 % 10),
               (unsigned long)snap.sweep_us,
               g_events.learning ? "LEARNING ('s' saves)" : "frozen ('r' redo)",
               (unsigned long)g_config.version,
               (unsigned long)g_events.events_on, (unsigned long)g_events.events_off);
    log_printf("idx |   val |   min |   max |   rng |  std | on");
    log_printf("----+-------+-------+-------+-------+------+---");
    for (int i = 0; i < PHOTON_ACTIVE_SENSORS; i++) {
        if ((g_events.disabled_mask >> i) & 1u) {
            log_printf("%3d |     - |     - |     - |     - |    - | disabled", i);
            continue;
        }
        uint16_t mn = snap.min[i], mx = snap.max[i];
        unsigned rng = mx > mn ? (unsigned)(mx - mn) : 0;
        log_printf("%3d | %5u | %5u | %5u | %5u | %4u | %d", i, snap.value[i],
                   mn == 0xFFFF ? 0 : mn, mx, rng, isqrt32(snap.var[i]),
                   g_events.note_on[i] ? 1 : 0);
    }
}

static void cal_enter(void) {
    photon_cmd_t c1 = { .op = PHOTON_CMD_RESET_CAL };
    photon_cmd_t c2 = { .op = PHOTON_CMD_CAL_LEARN, .a = 1 };
    push_core1_cmd(&c1);
    push_core1_cmd(&c2);
    log_info("CALIBRATION: play every key one at a time, full swing.");
    log_info("Enter shows the table; 's' freezes+saves, 'x' aborts (freeze only).");
}

static void cal_freeze(bool save) {
    photon_cmd_t c = { .op = PHOTON_CMD_CAL_LEARN, .a = 0 };
    push_core1_cmd(&c);
    if (save) {
        config_store_save();
        log_info("calibration frozen and saved");
    } else {
        log_info("calibration frozen (not saved)");
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
    {   // Drain leftovers from any earlier session: a stale first sample
        // would anchor t0 seconds in the past and corrupt the time axis.
        photon_trace_sample_t stale;
        while (trace_ring_pop(&stale)) { }
    }
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
    } else if (strcmp(cmd, "r") == 0 && C.sensor_role) {
        cal_enter();
    } else if (strcmp(cmd, "s") == 0 && C.sensor_role) {
        cal_freeze(true);
    } else if (strcmp(cmd, "x") == 0 && C.sensor_role) {
        cal_freeze(false);
    } else if (strcmp(cmd, "id") == 0) {
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        pico_get_unique_board_id_string(serial, sizeof serial);
        log_printf("PHOTON native fw | role=%s | addr=%u | banks=%s | cfg v%lu | hw %s",
                   C.is_bridge ? "bridge" : "sensor-node",
                   C.is_bridge ? 0 : g_config.node_id,
                   C.sensor_role ? "present" : "none",
                   (unsigned long)g_config.version, serial);
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
    } else if (strcmp(cmd, "test") == 0 && a1 != NULL) {
        // Pseudorandom load generator for loss validation: events flow the
        // full path and the bridge's per-node gaps/malformed counters plus
        // node evt totals prove (or disprove) zero loss.
        uint16_t rate = strcmp(a1, "stop") == 0 ? 0 : (uint16_t)atoi(a1);
        if (rate > 5000) {
            log_note("test: max 5000 ev/s");
        } else if (C.is_bridge) {
            uint8_t p[3];
            memcpy(p, &rate, 2);
            p[2] = 1;  // kind: continuous rate
            uint8_t dst = a2 ? (uint8_t)atoi(a2) : PHOTON_ADDR_BROADCAST;
            protocol_bridge_request(PHOTON_FT_TEST_BURST, dst, p, 3);
            log_info("test load %u ev/s -> %u (watch 'nodes': gaps/malformed must stay 0)",
                     rate, dst);
        } else if (C.sensor_role) {
            photon_cmd_t cmdm = { .op = PHOTON_CMD_TEST_RATE, .a = rate };
            push_core1_cmd(&cmdm);
            log_info("local test load %u ev/s", rate);
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
        if (strcmp(a1, "start") == 0 && C.sensor_role) {
            cal_enter();
        } else if (strcmp(a1, "save") == 0) {
            if (C.is_bridge) {
                // Optional node id: commit one board instead of the whole
                // bus, so a single miscalibrated board can be redone without
                // disturbing calibrations that are already good.
                int only = a2 ? atoi(a2) : 0;
                const photon_node_slot_t *t = protocol_node_table();
                int queued = 0, dropped = 0;
                for (int id = 1; id <= PHOTON_MAX_NODE_ID; id++) {
                    if (only && id != only) {
                        continue;
                    }
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
                cal_freeze(true);
            }
        } else if (strcmp(a1, "reset") == 0) {
            if (C.is_bridge) {
                uint8_t p[5] = { PHOTON_CAL_IDX_ALL, 0, 0, 0, 0 };
                uint8_t dst = a2 ? (uint8_t)atoi(a2) : PHOTON_ADDR_BROADCAST;
                protocol_bridge_request(PHOTON_FT_CAL_SET, dst, p, 5);
                log_info("calibration reset -> %s (learning ON there)",
                         a2 ? a2 : "ALL nodes");
            } else if (C.sensor_role) {
                photon_cmd_t cmdm = { .op = PHOTON_CMD_RESET_CAL };
                push_core1_cmd(&cmdm);
                log_info("calibration reset");
            }
        }
    } else if ((strcmp(cmd, "disable") == 0 || strcmp(cmd, "enable") == 0) && a1 != NULL) {
        int idx = atoi(a1);
        if (C.is_bridge) {
            // Bridge form: mask a GLOBAL sensor index out of the note map
            // (unpopulated slots on remote boards). Saved immediately.
            if (idx >= 0 && idx < (int)PHOTON_GLOBAL_SENSORS) {
                if (cmd[0] == 'd') {
                    g_config.global_disabled[idx / 8] |= 1u << (idx % 8);
                } else {
                    g_config.global_disabled[idx / 8] &= ~(1u << (idx % 8));
                }
                midi_map_init();
                config_store_save();
                log_info("global sensor %d %sd (saved)", idx, cmd);
            } else {
                log_note("%s: global index 0-%d", cmd, PHOTON_GLOBAL_SENSORS - 1);
            }
        } else if (!C.sensor_role) {
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
    } else if (strcmp(cmd, "rate") == 0 && a1 != NULL) {
        uint32_t hz;
        if (strcmp(a1, "max") == 0) {
            hz = 0xFFFF;  // unthrottled
        } else {
            int v = atoi(a1);
            hz = (v >= 50 && v <= 2000) ? (uint32_t)v : 0;
        }
        if (hz == 0) {
            log_note("rate: 50-2000 or 'max'");
        } else if (C.is_bridge) {
            uint8_t dst = a2 ? (uint8_t)atoi(a2) : PHOTON_ADDR_BROADCAST;
            send_nodectl(3, (uint16_t)hz, dst);
            log_info("scan rate %s -> node %u (persisted there; brief drop-out"
                     " while it writes flash)", a1, dst);
        } else if (!C.sensor_role) {
            log_note("rate: no scanner on this board");
        } else {
            photon_cmd_t cmdm = { .op = PHOTON_CMD_SCAN_RATE, .a = hz };
            push_core1_cmd(&cmdm);
            g_config.scan_rate_hz = (uint16_t)hz;
            config_store_save();
            log_info("scan rate -> %s (saved)", hz == 0xFFFF ? "max" : a1);
        }
    } else if (strcmp(cmd, "settle") == 0 && a1 != NULL) {
        // Bench A/B knob: emitter settle dominates emitter on-time and caps
        // the max scan rate. Deliberately NOT persisted — a bad value would
        // survive a reboot and quietly degrade every reading.
        int us = atoi(a1);
        if (us < 5 || us > 500) {
            log_note("settle: 5-500 us");
        } else if (C.is_bridge) {
            uint8_t dst = a2 ? (uint8_t)atoi(a2) : PHOTON_ADDR_BROADCAST;
            send_nodectl(5, (uint16_t)us, dst);
            log_info("settle %d us -> node %u (not persisted; reboot restores %d)",
                     us, dst, PHOTON_SETTLE_US);
        } else if (C.sensor_role) {
            photon_cmd_t cmdm = { .op = PHOTON_CMD_SETTLE, .a = (uint32_t)us };
            push_core1_cmd(&cmdm);
            log_info("settle -> %d us (not persisted)", us);
        }
    } else if (strcmp(cmd, "localmidi") == 0 && a1 != NULL) {
        if (!C.sensor_role) {
            log_note("localmidi: bridge always owns its MIDI");
        } else {
            g_config.local_midi = strcmp(a1, "on") == 0 ? 1 : 0;
            config_store_save();
            log_info("local USB-MIDI %s (saved) — events %s",
                     g_config.local_midi ? "on" : "off",
                     g_config.local_midi ? "play here when a host is attached"
                                         : "always go to the bus/bridge");
        }
    } else if (strcmp(cmd, "velrange") == 0) {
        // A harpsichord's loudness is independent of touch, so the useful
        // range is a taste question tuned against a real sample library.
        if (a1 == NULL || a2 == NULL) {
            log_note("velrange <min> <max> (1-127, min <= max; now %u-%u)",
                     (unsigned)g_config.vel_out_min,
                     (unsigned)g_config.vel_out_max);
        } else {
            int lo = atoi(a1), hi = atoi(a2);
            if (lo < 1 || hi > 127 || lo > hi) {
                log_note("velrange: 1-127, min <= max");
            } else {
                // Applied here AND broadcast: velocity is mapped by whoever
                // emits MIDI (the bridge normally, a 'localmidi on' node for
                // itself), and every board holds an identical copy.
                g_config.vel_out_min = (float)lo;
                g_config.vel_out_max = (float)hi;
                config_store_save();
                if (C.is_bridge) {
                    send_nodectl(6, (uint16_t)(lo | (hi << 8)),
                                 PHOTON_ADDR_BROADCAST);
                }
                log_info("velocity range -> %d-%d (saved%s)", lo, hi,
                         C.is_bridge ? "; broadcast, brief node drop-out while"
                                       " they write flash"
                                     : "");
            }
        }
    } else if (strcmp(cmd, "log") == 0 && a1 != NULL) {
        C.log_events = strcmp(a1, "off") != 0;
        log_info("event log + heartbeat %s", C.log_events ? "on" : "off");
    } else if (strcmp(cmd, "mode") == 0 && a1 != NULL) {
        uint8_t m = (uint8_t)atoi(a1);
        if (C.is_bridge) {
            if (m > PHOTON_SCAN_TWO_PHASE) {
                log_note("mode: 0=seq 1=parallel 2=two-phase");
            } else {
                uint8_t dst = a2 ? (uint8_t)atoi(a2) : PHOTON_ADDR_BROADCAST;
                send_nodectl(4, m, dst);
                log_info("scan mode %u -> node %u (persisted there)", m, dst);
            }
        } else if (C.sensor_role && m <= PHOTON_SCAN_TWO_PHASE) {
            photon_cmd_t cmdm = { .op = PHOTON_CMD_SCAN_MODE, .arg8 = m };
            push_core1_cmd(&cmdm);
            g_config.scan_mode = m;
            log_info("scan mode -> %u", m);
        }
    } else if (strcmp(cmd, "setid") == 0 && a1 != NULL) {
        int id = atoi(a1);
        if (C.is_bridge && a2 != NULL) {
            int newid = atoi(a2);
            if (newid >= 1 && newid <= PHOTON_MAX_NODE_ID) {
                send_nodectl(2, (uint16_t)newid, (uint8_t)id);
                log_info("setid: node %d -> id %d (effective on its reboot)",
                         id, newid);
            } else {
                log_note("setid <node> <newid>: newid 1-%d", PHOTON_MAX_NODE_ID);
            }
        } else if (id >= 1 && id <= PHOTON_MAX_NODE_ID) {
            g_config.node_id = (uint8_t)id;
            config_store_save();
            log_info("node id -> %d (takes effect on reboot)", id);
        }
    } else if (strcmp(cmd, "chmap") == 0) {
        // Deployment shape (single vs double manual) is pure runtime config:
        // whichever nodes answer discovery play, and this maps each manual
        // (pair of node ids) to a user-facing MIDI channel. 0/auto = legacy
        // formula (base + manual index).
        if (!C.is_bridge) {
            log_note("chmap: bridge-only command");
        } else if (a1 == NULL) {
            for (uint32_t m = 0; m < PHOTON_MAX_MANUALS; m++) {
                uint8_t uc = g_config.manual_channel[m];
                uint8_t wire = midi_map_channel_for_manual(m);
                log_printf("manual %lu (nodes %lu-%lu): channel %u%s",
                           (unsigned long)(m + 1),
                           (unsigned long)(m * 2 + 1), (unsigned long)(m * 2 + 2),
                           wire + 1, uc ? "" : " (auto)");
            }
        } else if (a2 != NULL) {
            int m = atoi(a1);
            int ch = strcmp(a2, "auto") == 0 ? 0 : atoi(a2);
            if (m >= 1 && m <= (int)PHOTON_MAX_MANUALS && ch >= 0 && ch <= 16) {
                g_config.manual_channel[m - 1] = (uint8_t)ch;
                midi_map_init();
                config_store_save();
                log_info("manual %d -> channel %s (saved)", m, ch ? a2 : "auto");
            } else {
                log_note("chmap: manual 1-%d, channel 1-16 or auto", PHOTON_MAX_MANUALS);
            }
        }
    } else if (strcmp(cmd, "reboot") == 0 && a1 != NULL && C.is_bridge) {
        send_nodectl(0, 0, (uint8_t)atoi(a1));
        log_info("reboot -> node %s", a1);
    } else if (strcmp(cmd, "bootsel") == 0 && a1 != NULL && C.is_bridge) {
        send_nodectl(1, 0, (uint8_t)atoi(a1));
        log_info("bootsel -> node %s (it will drop off the bus until reflashed)",
                 a1);
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
    // Input, with live character echo (terminals don't local-echo raw CDC).
    while (tud_cdc_available()) {
        uint8_t ch;
        if (tud_cdc_read(&ch, 1) != 1) {
            break;
        }
        if (ch == '\r' || ch == '\n') {
            tud_cdc_write("\r\n", 2);
            tud_cdc_write_flush();
            if (C.line_len > 0) {
                C.line[C.line_len] = 0;
                C.line_len = 0;
                handle_line(C.line);
            } else {
                print_table();  // bare Enter = live table, like the legacy console
            }
        } else if (ch == 0x7F || ch == '\b') {
            if (C.line_len > 0) {
                C.line_len--;
                tud_cdc_write("\b \b", 3);
                tud_cdc_write_flush();
            }
        } else if (C.line_len < sizeof C.line - 1) {
            C.line[C.line_len++] = (char)ch;
            tud_cdc_write(&ch, 1);
            tud_cdc_write_flush();
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

    // Legacy-style 5 s heartbeat (suppressed during traces and by 'log off').
    if (C.log_events && !C.trace_active && log_console_connected() &&
        time_reached(C.next_heartbeat_at)) {
        C.next_heartbeat_at = make_timeout_time_ms(5000);
        if (C.sensor_role) {
            uint32_t hz10 = g_scan_ctl.period_us ? 10000000u / g_scan_ctl.period_us : 0;
            log_printf("[STAT] %lu.%lu Hz | evt on/off %lu/%lu | cal %s | midi %lu/%lu",
                       (unsigned long)(hz10 / 10), (unsigned long)(hz10 % 10),
                       (unsigned long)g_events.events_on,
                       (unsigned long)g_events.events_off,
                       g_events.learning ? "LEARNING" : "frozen",
                       (unsigned long)midi_map_notes_on_sent(),
                       (unsigned long)midi_map_notes_off_sent());
        } else {
            const photon_node_slot_t *t = protocol_node_table();
            int alive = 0;
            uint32_t events = 0;
            for (int id = 1; id <= PHOTON_MAX_NODE_ID; id++) {
                if (t[id].alive) {
                    alive++;
                }
                events += t[id].events_rx;
            }
            log_printf("[STAT] nodes=%d | events=%lu | midi %lu/%lu | cycles=%lu",
                       alive, (unsigned long)events,
                       (unsigned long)midi_map_notes_on_sent(),
                       (unsigned long)midi_map_notes_off_sent(),
                       (unsigned long)protocol_poll_cycles());
        }
    }
}
