// OPTIONAL touchscreen support for the main controller board 001 (see
// lcd.h): CST328 capacitive touch controller (Hynitron) on I2C1, pins
// PHOTON_TOUCH_* in board_config.h. LVGL has no driver for it; this is the
// register protocol of Waveshare's reference driver, plus two fixes measured
// on the hardware (INT settle time, lift window; see touch.c).
#ifndef PHOTON_TS_TOUCH_H
#define PHOTON_TS_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#define TOUCH_MAX_POINTS 5

typedef struct {
    uint16_t x, y;      // panel-native portrait coordinates, 0-239 / 0-319
    uint8_t strength;
} touch_point_t;

// Reset the controller and check its 0xCACA signature. Returns detection;
// the reported resolution and firmware words are kept for touch_info().
bool touch_init(void);
bool touch_present(void);
void touch_info(uint16_t *res_x, uint16_t *res_y, uint32_t *boot_time);

// Current points: count (0 = no finger), or -1 on an I2C error.
int touch_read(touch_point_t *pts, int max);

// Points seen by the most recent LVGL poll (multi-touch, for callers).
int touch_last(touch_point_t *pts, int max);

// LVGL pointer input read callback (single point, native coordinates;
// LVGL applies the display rotation).
void touch_lvgl_read(lv_indev_t *indev, lv_indev_data_t *data);

// Diagnostics.
uint32_t touch_errors(void);
uint32_t touch_int_edges(void);   // falling edges seen on TP_INT
// True once per TP_INT edge: the controller has a fresh report, so the main
// loop runs LVGL's input read now instead of at the next 10 ms poll.
bool touch_take_int(void);

// Debug trace: set touch_dbg_on, then drain with touch_dbg_pop().
enum { TDBG_POLL, TDBG_INT, TDBG_STATE, TDBG_LVGL };
typedef struct {
    uint32_t t_us;
    uint8_t type;
    int16_t a, b, c;   // POLL: n,x,y  STATE: pressed,x,y  LVGL: event code
} touch_dbg_t;
extern bool touch_dbg_on;
void touch_dbg_push(uint8_t type, int16_t a, int16_t b, int16_t c);
bool touch_dbg_pop(touch_dbg_t *out);

#endif
