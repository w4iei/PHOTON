// SETUP.TXT on the main controller board, core 0: a few seconds after
// power-on, once discovery has found the boards, read every board's
// calibration table (the pages 'cal compare' shows), add the bridge's own
// settings, and hand the text to the recorder, which saves it next to this
// power-on's recordings. Read once per power-on: a change made later shows
// in the next power-on's file.
#ifndef PHOTON_SETUP_LOG_H
#define PHOTON_SETUP_LOG_H

#include <stdbool.h>

#include "comms/frame.h"

// Main controller board only (it carries the recorder).
void setup_log_init(void);
void setup_log_task(void);
// Response sink hook: true = a page this module asked for, consumed.
bool setup_log_on_response(const photon_frame_t *f);

#endif
