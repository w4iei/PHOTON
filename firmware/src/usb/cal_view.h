// Knee calibration on the console: the live keyboard view (every key of
// every manual being calibrated, green once its pluck is captured), the
// before/after table ('cal compare') and the swing dump ('cal dump').
// Works the same on a sensor board's own console (its keys) and on the
// bridge (all boards, over the bus).
#ifndef PHOTON_CAL_VIEW_H
#define PHOTON_CAL_VIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "comms/frame.h"

void calview_init(bool is_bridge, bool sensor_role);
void calview_set_bus_master(bool on);

// Live view on/off. fresh: a calibration is starting (forget cached node
// states and coupler evidence).
void calview_show(bool on, bool fresh);
bool calview_showing(void);

// View or dump running: event lines and the heartbeat stay off the screen.
bool calview_busy(void);

// Console loop: redraws the view (with the line being typed at its prompt),
// polls remote nodes, and pumps compare/dump jobs.
void calview_task(const char *line, size_t line_len);

// Bridge: calibration replies from nodes. True if consumed.
bool calview_on_response(const photon_frame_t *f);

// Before/after table for these node ids (n = 0: this board, or every node
// on the bus from the bridge). Remote nodes are asked after delay_ms, so a
// board still writing its flash after 'cal save' has finished.
void calview_compare(const uint8_t *ids, int n, uint32_t delay_ms);

// Captured swings as CSV blocks for tools/cal_dump.py. key < 0: every key.
void calview_dump(uint8_t node, int key);

#endif
