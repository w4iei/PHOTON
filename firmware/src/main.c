// PHOTON native firmware — one image for every board.
//
// Boot: probe the TLA2518 banks. Any bank present => sensor role: core 1
// runs the SRAM-resident scan/event loop, core 0 answers the bus. Zero
// banks => this is the main controller (or future endpoint) board: core 0
// becomes the bus master/bridge (poll cycle + USB-MIDI), core 1 stays off.
//
// The whole binary runs copy-to-RAM (see CMakeLists), so no code fetch ever
// touches flash/XIP at runtime — the scan loop cannot be stalled by core-0
// flash writes, by construction.
#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "board_config.h"
#include "bridge/midi_map.h"
#include "comms/protocol.h"
#include "comms/transport.h"
#include "config/config_store.h"
#include "core1/events.h"
#include "core1/scan.h"
#include "core1/tla2518.h"
#include "ipc/rings.h"
#include "usb/cdc_console.h"
#include "util/log.h"

int main(void) {
    // Deterministic peripheral clock: UART/SPI dividers derive from 150 MHz.
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys), clock_get_hz(clk_sys));

    rings_init();
    config_store_init();

    int banks_found = tla2518_init_and_probe();
    bool sensor_role = banks_found > 0;
    bool is_bridge = !sensor_role;
    uint8_t own_addr = is_bridge ? PHOTON_ADDR_BRIDGE : g_config.node_id;

    transport_init(is_bridge, own_addr, protocol_on_frame);
    protocol_init(is_bridge, own_addr);
    midi_map_init();
    if (is_bridge) {
        protocol_set_event_sink(midi_map_handle_event);
    }
    console_init(is_bridge, sensor_role);

    if (sensor_role) {
        events_init(g_config.local_disabled_mask);
        for (int i = 0; i < PHOTON_MAX_SENSORS; i++) {
            if (g_config.cal_min[i] != 0xFFFF || g_config.cal_max[i] != 0) {
                events_seed_cal((uint8_t)i, g_config.cal_min[i], g_config.cal_max[i]);
            }
        }
        g_scan_ctl.mode = g_config.scan_mode <= PHOTON_SCAN_TWO_PHASE
                              ? g_config.scan_mode
                              : PHOTON_SCAN_PARALLEL;
        multicore_launch_core1(scan_core1_entry);
        config_store_set_core1_running(true);
    }

    tud_init(0);

    bool announced = false;
    for (;;) {
        tud_task();
        transport_task();
        protocol_task();
        console_task();

        if (!announced && log_console_connected()) {
            announced = true;
            log_info("PHOTON native fw | %s role | banks=%d | addr=%u | cfg v%lu%s",
                     is_bridge ? "bridge" : "sensor-node", banks_found, own_addr,
                     (unsigned long)g_config.version,
                     g_config_from_flash ? "" : " (defaults, uncalibrated)");
        } else if (announced && !log_console_connected()) {
            announced = false;
        }
    }
}
