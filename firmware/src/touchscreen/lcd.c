#include "touchscreen/lcd.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "pico/time.h"

#include "board_config.h"

// Panel-specific register values for the Waveshare 2.8" ST7789T3, from
// Waveshare's reference driver.
// Loaded after LVGL's generic ST7789 init; MADCTL, COLMOD, INVON, SLPOUT and
// DISPON stay with the LVGL driver.
static const uint8_t waveshare_st7789t3[] = {
    0xB2, 5, 0x0B, 0x0B, 0x00, 0x33, 0x35,   // porch
    0xB7, 1, 0x11,                           // gate voltages
    0xBB, 1, 0x35,                           // VCOM
    0xC0, 1, 0x2C,
    0xC2, 1, 0x01,
    0xC3, 1, 0x0D,                           // VRH
    0xC4, 1, 0x20,                           // VDV
    0xC6, 1, 0x13,                           // frame rate
    0xD0, 2, 0xA4, 0xA1,                     // power
    0xD6, 1, 0xA1,
    0xE0, 14, 0xF0, 0x06, 0x0B, 0x0A, 0x09, 0x26, 0x29, 0x33, 0x41, 0x18, 0x16, 0x15, 0x29, 0x2D,
    0xE1, 14, 0xF0, 0x04, 0x08, 0x08, 0x07, 0x03, 0x28, 0x32, 0x40, 0x3B, 0x19, 0x18, 0x2A, 0x2E,
    LV_LCD_CMD_DELAY_MS, LV_LCD_CMD_EOF,
};

#define BUF_LINES 40
static uint16_t buf_a[PHOTON_LCD_HEIGHT * BUF_LINES] __attribute__((aligned(4)));
static uint16_t buf_b[PHOTON_LCD_HEIGHT * BUF_LINES] __attribute__((aligned(4)));

static lv_display_t *disp;
static int dma_ch = -1;
static dma_channel_config dma_cfg;
static volatile bool busy;
static uint32_t spi_hz;
static uint32_t flushes;

static void spi_bits(uint8_t bits) {
    spi_set_format(PHOTON_LCD_SPI, bits, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

// End of a pixel transfer: the DMA has emptied the buffer into the FIFO; let
// the FIFO drain (a few us), then release the bus and tell LVGL.
static void dma_irq(void) {
    dma_hw->ints0 = 1u << dma_ch;
    while (spi_is_busy(PHOTON_LCD_SPI)) {
    }
    gpio_put(PHOTON_LCD_CS, 1);
    spi_bits(8);
    busy = false;
    flushes++;
    lv_display_flush_ready(disp);
}

static void wait_idle(void) {
    while (busy) {
        tight_loop_contents();
    }
}

static void send_cmd(lv_display_t *d, const uint8_t *cmd, size_t cmd_size,
                     const uint8_t *param, size_t param_size) {
    (void)d;
    wait_idle();
    gpio_put(PHOTON_LCD_DC, 0);
    gpio_put(PHOTON_LCD_CS, 0);
    spi_write_blocking(PHOTON_LCD_SPI, cmd, cmd_size);
    if (param_size > 0) {
        gpio_put(PHOTON_LCD_DC, 1);
        spi_write_blocking(PHOTON_LCD_SPI, param, param_size);
    }
    gpio_put(PHOTON_LCD_CS, 1);
}

// LVGL renders RGB565 little-endian; 16-bit SPI frames put the high byte on
// the wire first, which is the order the panel wants.
static void send_color(lv_display_t *d, const uint8_t *cmd, size_t cmd_size,
                       uint8_t *param, size_t param_size) {
    (void)d;
    wait_idle();
    gpio_put(PHOTON_LCD_DC, 0);
    gpio_put(PHOTON_LCD_CS, 0);
    spi_write_blocking(PHOTON_LCD_SPI, cmd, cmd_size);
    gpio_put(PHOTON_LCD_DC, 1);
    spi_bits(16);
    busy = true;
    dma_channel_configure(dma_ch, &dma_cfg, &spi_get_hw(PHOTON_LCD_SPI)->dr, param,
                          param_size / 2, true);
}

static void (*pump_fn)(void);

static void delay_cb(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        sleep_ms(1);
        if (pump_fn) {
            pump_fn();
        }
    }
}

void lcd_set_backlight(uint8_t percent) {
    if (percent > 100) {
        percent = 100;
    }
    pwm_set_gpio_level(PHOTON_LCD_BL, (uint16_t)(percent * 100));  // wrap 9999
}

void lcd_set_flip(bool flipped) {
    lv_display_set_rotation(disp, flipped ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_90);
}

void lcd_set_invert(bool invert) { lv_st7789_set_invert(disp, invert); }
uint32_t lcd_spi_hz(void) { return spi_hz; }
uint32_t lcd_flushes(void) { return flushes; }

lv_display_t *lcd_init(bool flipped, bool invert, uint8_t brightness, void (*pump)(void)) {
    pump_fn = pump;
    gpio_init(PHOTON_LCD_CS);
    gpio_set_dir(PHOTON_LCD_CS, GPIO_OUT);
    gpio_put(PHOTON_LCD_CS, 1);
    gpio_init(PHOTON_LCD_DC);
    gpio_set_dir(PHOTON_LCD_DC, GPIO_OUT);
    gpio_put(PHOTON_LCD_DC, 1);
    gpio_init(PHOTON_LCD_RST);
    gpio_set_dir(PHOTON_LCD_RST, GPIO_OUT);

    // Backlight off until the first frame is drawn: PWM 15 kHz.
    gpio_set_function(PHOTON_LCD_BL, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PHOTON_LCD_BL);
    pwm_set_wrap(slice, 9999);
    pwm_set_gpio_level(PHOTON_LCD_BL, 0);
    pwm_set_enabled(slice, true);

    spi_hz = spi_init(PHOTON_LCD_SPI, PHOTON_LCD_SPI_HZ);
    spi_bits(8);
    gpio_set_function(PHOTON_LCD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PHOTON_LCD_MOSI, GPIO_FUNC_SPI);

    dma_ch = dma_claim_unused_channel(true);
    dma_cfg = dma_channel_get_default_config(dma_ch);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_16);
    channel_config_set_dreq(&dma_cfg, spi_get_dreq(PHOTON_LCD_SPI, true));
    channel_config_set_read_increment(&dma_cfg, true);
    channel_config_set_write_increment(&dma_cfg, false);
    dma_channel_set_irq0_enabled(dma_ch, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq);
    irq_set_enabled(DMA_IRQ_0, true);

    // Hardware reset (ST7789: >= 10 us low, >= 120 ms before SLPOUT).
    gpio_put(PHOTON_LCD_RST, 1);
    delay_cb(10);
    gpio_put(PHOTON_LCD_RST, 0);
    delay_cb(15);
    gpio_put(PHOTON_LCD_RST, 1);
    delay_cb(150);

    lv_delay_set_cb(delay_cb);
    disp = lv_st7789_create(PHOTON_LCD_WIDTH, PHOTON_LCD_HEIGHT, LV_LCD_FLAG_NONE, send_cmd, send_color);
    lv_st7789_send_cmd_list(disp, waveshare_st7789t3);
    lv_st7789_set_invert(disp, invert);
    lv_display_set_buffers(disp, buf_a, buf_b, sizeof buf_a, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lcd_set_flip(flipped);

    // First frame (the screen background) before the light comes on, so
    // the panel's power-on garbage is never visible.
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_refr_now(disp);
    wait_idle();
    lcd_set_backlight(brightness);
    return disp;
}

// Blocking solid fill that bypasses LVGL and the DMA: first-light checks.
static void cmd_bytes(uint8_t cmd, const uint8_t *p, size_t n) {
    send_cmd(disp, &cmd, 1, p, n);
}

void lcd_fill_raw(uint16_t rgb565) {
    wait_idle();
    uint16_t w = (uint16_t)lv_display_get_horizontal_resolution(disp);
    uint16_t h = (uint16_t)lv_display_get_vertical_resolution(disp);
    uint8_t ca[4] = { 0, 0, (uint8_t)((w - 1) >> 8), (uint8_t)(w - 1) };
    uint8_t ra[4] = { 0, 0, (uint8_t)((h - 1) >> 8), (uint8_t)(h - 1) };
    cmd_bytes(0x2A, ca, 4);
    cmd_bytes(0x2B, ra, 4);
    uint8_t ramwr = 0x2C;
    gpio_put(PHOTON_LCD_DC, 0);
    gpio_put(PHOTON_LCD_CS, 0);
    spi_write_blocking(PHOTON_LCD_SPI, &ramwr, 1);
    gpio_put(PHOTON_LCD_DC, 1);
    spi_bits(16);
    static uint16_t row[PHOTON_LCD_HEIGHT];
    for (int i = 0; i < PHOTON_LCD_HEIGHT; i++) {
        row[i] = rgb565;
    }
    for (int y = 0; y < h; y++) {
        spi_write16_blocking(PHOTON_LCD_SPI, row, w);
    }
    spi_bits(8);
    gpio_put(PHOTON_LCD_CS, 1);
}
