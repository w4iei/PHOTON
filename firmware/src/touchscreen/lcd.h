// OPTIONAL touchscreen support for the main controller board 001 (Waveshare
// 2.8" ST7789T3 + CST328 on the FPC). Not part of the PHOTON image; built only
// with -DPHOTON_TOUCHSCREEN=ON, which also fetches LVGL (see CMakeLists.txt).
//
// The panel runs on LVGL's own ST7789 driver; this file supplies the
// transport: SPI1 for commands, DMA (16-bit frames, so no byte swapping) for
// pixels, completion in the DMA interrupt. Pins: PHOTON_LCD_* in
// board_config.h. The panel never answers register reads on this board
// (MISO reads the microSD socket's pull-ups), so first light is by eye:
// lcd_fill_raw() paints the whole screen without LVGL.
#ifndef PHOTON_TS_LCD_H
#define PHOTON_TS_LCD_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

// Reset the panel, create the LVGL display (landscape; `flipped` = rotated
// 180 degrees) and switch the backlight on. Call after lv_init(). `pump`
// (may be NULL) runs every millisecond of the power-on delays, so USB stays
// serviced during the ~0.7 s bring-up.
lv_display_t *lcd_init(bool flipped, bool invert, uint8_t brightness, void (*pump)(void));

void lcd_set_backlight(uint8_t percent);
void lcd_set_flip(bool flipped);
void lcd_set_invert(bool invert);

// Diagnostics.
uint32_t lcd_spi_hz(void);
uint32_t lcd_flushes(void);
void lcd_fill_raw(uint16_t rgb565);   // bypasses LVGL and the DMA

#endif
