// LVGL v9.3 configuration for the OPTIONAL 001 touchscreen support (only
// used with -DPHOTON_TOUCHSCREEN=ON). Overrides only; everything else takes
// LVGL's defaults (lv_conf_internal.h).
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_USE_OS LV_OS_NONE
#define LV_MEM_SIZE (96 * 1024U)
#define LV_DEF_REFR_PERIOD 10
#define LV_DPI_DEF 143            // 320x240 on a 2.8" diagonal

// Panel driver: LVGL's generic MIPI-DBI ST7789; lcd.c supplies the SPI transport.
#define LV_USE_ST7789 1

#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif
