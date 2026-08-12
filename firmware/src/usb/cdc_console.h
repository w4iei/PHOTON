// Interactive console on the CDC interface, both roles. Keeps the v1 wire
// conventions where host tooling depends on them: `# LOG` prefixes and the
// BEGIN_TRACE / t,adc CSV / END_TRACE capture contract.
#ifndef PHOTON_CDC_CONSOLE_H
#define PHOTON_CDC_CONSOLE_H

#include <stdbool.h>

#include "comms/frame.h"

void console_init(bool is_bridge, bool sensor_role);
void console_task(void);

// Bridge-side: responses to console-issued bulk requests arrive here.
void console_on_bridge_response(const photon_frame_t *f);

#endif
