// Interactive console on the CDC interface, both roles. Keeps the v1 wire
// conventions where host tooling depends on them: `# LOG` prefixes and the
// BEGIN_TRACE / t,adc CSV / END_TRACE capture contract.
#ifndef PHOTON_CDC_CONSOLE_H
#define PHOTON_CDC_CONSOLE_H

#include <stdbool.h>

#include "comms/frame.h"

#include "ipc/rings.h"

void console_init(bool is_bridge, bool sensor_role);
void console_task(void);

// Full attach banner: identity, project link, config summary, command help.
// Printed by main on every console (DTR) connect edge.
void console_print_banner(int banks_found);

// Bridge-side: responses to console-issued bulk requests arrive here.
void console_on_bridge_response(const photon_frame_t *f);

// Live per-event log line ("[EVT] ON  s=12 F#2(42) vel=97 dt=8.4ms"),
// suppressed by the 'log off' console toggle or when no terminal attached.
void console_print_event(uint8_t node_id, const photon_event_t *ev,
                         int16_t note, uint8_t velocity, uint8_t channel);

#endif
