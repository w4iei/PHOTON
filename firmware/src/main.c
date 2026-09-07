// PHOTON native firmware — one image for every board.
//
// Boot: probe the TLA2518 banks. Any bank present => sensor role: core 1
// runs the SRAM-resident scan/event loop, core 0 answers the bus. Zero
// banks => this is the main controller (or future endpoint) board: core 0
// becomes the bus master/bridge (poll cycle + USB-MIDI), core 1 stays off.
// A sensor board with 'master on' saved takes the bus-master role as well
// (instruments built without a main controller board) once a USB host has
// enumerated it and the wire has stayed silent: core 0 then polls the other
// boards and owns USB-MIDI while core 1 keeps scanning its own keys.
//
// The whole binary runs copy-to-RAM (see CMakeLists), so no code fetch ever
// touches flash/XIP at runtime — the scan loop cannot be stalled by core-0
// flash writes, by construction.
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "board_config.h"
#include "bridge/midi_map.h"
#include "bridge/recorder.h"
#include "comms/protocol.h"
#include "comms/transport.h"
#include "config/config_store.h"
#include "core1/events.h"
#include "core1/scan.h"
#include "core1/tla2518.h"
#include "ipc/rings.h"
#include "usb/cdc_console.h"
#include "util/log.h"

// 'master on' sensor board claiming the bus at runtime: same wiring as the
// main controller board's boot path, minus the pinout/termination/recorder
// (board identity does not change), plus its own scanner in the note map.
static void become_bus_master(void) {
    transport_set_own_addr(PHOTON_ADDR_BRIDGE);
    protocol_init(true, PHOTON_ADDR_BRIDGE);
    protocol_set_self_node_id(g_config.node_id);
    protocol_set_event_sink(midi_map_handle_event);
    protocol_set_node_down_cb(midi_map_release_node);
    protocol_set_response_sink(console_on_bridge_response);
    midi_map_init();
    console_set_bus_master(true);
    log_note("USB host attached and no bus master heard: this board now runs the bus "
             "(node id %u keeps its place in the note map)", g_config.node_id);
}

int main(void) {
    // Deterministic peripheral clock: UART/SPI dividers derive from 150 MHz.
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys));

    rings_init();
    config_store_init();

    int banks_found = tla2518_init_and_probe();
    if (banks_found == 0) {
        // Guard against the field-observed first-transaction SPI glitch
        // hitting the probe itself: a sensor board misprobing as "no banks"
        // would come up as a second bus master. One settled retry.
        sleep_ms(100);
        banks_found = tla2518_init_and_probe();
    }
    bool sensor_role = banks_found > 0;
    // Board identity follows capability: no banks => main controller board
    // pinout, terminated endpoint, microSD socket, bus master from boot. A
    // sensor board with 'master on' boots as a node and claims the master
    // role from the main loop once a USB host has enumerated it and the
    // wire has been silent (become_bus_master).
    bool main_board = !sensor_role;
    bool is_bridge = main_board;
    uint8_t own_addr = is_bridge ? PHOTON_ADDR_BRIDGE : g_config.node_id;

    transport_init(/*use_host_pinout=*/main_board, /*terminate=*/main_board,
                   own_addr, protocol_on_frame);
    protocol_init(is_bridge, own_addr);
    midi_map_init();
    // Every role gets the MIDI sink: the bridge feeds it from poll batches,
    // and a sensor node feeds it its OWN events while no bridge is polling
    // (standalone-over-USB mode). midi_out is silent unless a host mounted
    // the MIDI interface, so this costs nothing on a headless bus node.
    protocol_set_event_sink(midi_map_handle_event);
    if (is_bridge) {
        protocol_set_node_down_cb(midi_map_release_node);
    }
    protocol_set_response_sink(console_on_bridge_response);
    console_init(is_bridge, sensor_role);

    if (sensor_role) {
        events_init(g_config.local_disabled_mask);
        bool have_cal = false;
        for (int i = 0; i < PHOTON_MAX_SENSORS; i++) {
            if (g_config.cal_min[i] != 0xFFFF || g_config.cal_max[i] != 0) {
                events_seed_cal((uint8_t)i, g_config.cal_min[i], g_config.cal_max[i]);
                have_cal = true;
            }
        }
        // Saved calibration => frozen thresholds for performance (continuous
        // learning is polluted by adjacent-key cross-illumination). No saved
        // calibration => boot straight into learning so the board is usable,
        // with the console banner flagging the uncalibrated state.
        g_events.learning = !have_cal;
        g_scan_ctl.mode = g_config.scan_mode <= PHOTON_SCAN_TWO_PHASE
                              ? g_config.scan_mode
                              : PHOTON_SCAN_PARALLEL;
        g_scan_ctl.rate_hz = g_config.scan_rate_hz == 0
                                 ? PHOTON_DEFAULT_SCAN_RATE_HZ
                             : g_config.scan_rate_hz == 0xFFFF
                                 ? 0
                                 : g_config.scan_rate_hz;
        multicore_launch_core1(scan_core1_entry);
        config_store_set_core1_running(true);
    } else {
        // Bridge: core 1 is otherwise idle, so it owns the microSD recorder
        // and every blocking card transfer. The image runs from SRAM, so
        // core-0 config saves need no park (core1_running stays false).
        static uint32_t recorder_stack[2048];  // 8 KB: FatFs + SD driver
        recorder_init();
        multicore_launch_core1_with_stack(recorder_core1_entry, recorder_stack,
                                          sizeof recorder_stack);
    }

    tud_init(0);

    bool announced = false;
    bool zero_fault_handled = false;
    uint32_t quiet_rx = 0;
    absolute_time_t quiet_since = get_absolute_time();
    for (;;) {
        tud_task();
        if (sensor_role && !is_bridge) {
            // A host is truly present only when mounted and not suspended
            // (a pulled cable is a *suspend*, not an unmount — mounted state
            // survives cable removal).
            bool host = tud_midi_mounted() && !tud_suspended();
            // Local USB-MIDI only when the config flag allows it AND a host
            // is present. A master owns its MIDI unconditionally instead.
            protocol_set_local_delivery(g_config.local_midi != 0 && host);
            // 'master on': claim the bus once this board has the host and
            // the wire has been silent for PHOTON_MASTER_QUIET_MS. Nodes
            // never transmit unsolicited, so any received frame means a
            // master is already running and this board stays a node — the
            // board without the USB cable in a bridgeless pair, or any
            // board next to a main controller board.
            if (g_config.bus_master != 0) {
                uint32_t rx = transport_stats()->rx_frames;
                absolute_time_t now = get_absolute_time();
                if (!host || rx != quiet_rx) {
                    quiet_rx = rx;
                    quiet_since = now;
                } else if (absolute_time_diff_us(quiet_since, now) >=
                           (int64_t)PHOTON_MASTER_QUIET_MS * 1000) {
                    become_bus_master();
                    is_bridge = true;
                }
            }
        }
        transport_task();
        protocol_task();
        console_task();

        // Startup zero-fault escalation (legacy microcontroller.reset()
        // behavior): if the array still reads all-zero ~1 s in, reboot —
        // unless a console is attached, where a reboot loop would make
        // USB-only debugging (no SWD populated) impossible.
        if (sensor_role && g_scan_ctl.zero_fault && !zero_fault_handled) {
            zero_fault_handled = true;
            if (log_console_connected()) {
                log_note("sensor array reads all-zero after reinit — check SPI/power "
                         "(auto-reboot suppressed while console attached)");
            } else {
                sleep_ms(100);
                watchdog_reboot(0, 0, 100);
            }
        }

        if (!announced && log_console_connected()) {
            announced = true;
            console_print_banner(banks_found);
        } else if (announced && !log_console_connected()) {
            announced = false;
        }
    }
}
