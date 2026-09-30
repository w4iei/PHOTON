#include "touchscreen/touch.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/time.h"
#include "hardware/sync.h"

#include "board_config.h"

// 16-bit register addresses, sent big-endian.
#define REG_TOUCH_DATA   0xD000   // 27 bytes: 5 points, status at 0xD005
#define REG_TOUCH_COUNT  0xD005   // low nibble = number of fingers; write 0 to ack
#define REG_DEBUG_INFO   0xD101   // enter info mode (write, no data)
#define REG_NORMAL_MODE  0xD109
#define REG_INFO_TP_NTX  0xD1F4   // 24-byte info block through 0xD20B
#define REG_INFO_RES     0xD1F8
#define REG_INFO_BOOT    0xD1FC

#define I2C_TIMEOUT_US   3000

// --- debug trace (enable with touch_dbg_on): every non-idle poll, every INT
// edge, every LVGL pointer event, timestamped; the caller drains and prints it.
#define DBG_SLOTS 256
static touch_dbg_t dbg[DBG_SLOTS];
static volatile uint16_t dbg_head, dbg_tail;
bool touch_dbg_on;

void touch_dbg_push(uint8_t type, int16_t a, int16_t b, int16_t c) {
    if (!touch_dbg_on) {
        return;
    }
    uint32_t irq = save_and_disable_interrupts();
    uint16_t next = (uint16_t)((dbg_head + 1) % DBG_SLOTS);
    if (next != dbg_tail) {
        dbg[dbg_head] = (touch_dbg_t){ time_us_32(), type, a, b, c };
        dbg_head = next;
    }
    restore_interrupts(irq);
}

bool touch_dbg_pop(touch_dbg_t *out) {
    if (dbg_tail == dbg_head) {
        return false;
    }
    *out = dbg[dbg_tail];
    dbg_tail = (uint16_t)((dbg_tail + 1) % DBG_SLOTS);
    return true;
}

static volatile uint32_t int_edges;
static volatile bool int_pending;
static volatile uint32_t int_at_us;

static void int_irq(uint gpio, uint32_t events) {
    (void)gpio;
    (void)events;
    int_edges++;
    int_at_us = time_us_32();
    int_pending = true;
    touch_dbg_push(TDBG_INT, 0, 0, 0);
}

uint32_t touch_int_edges(void) { return int_edges; }

// INT falls slightly before the report registers are updated: a read in
// the first fraction of a millisecond still finds the previous (consumed,
// zero) state. Wait INT_SETTLE_US before acting on the edge.
#define INT_SETTLE_US 2000

bool touch_take_int(void) {
    if (!int_pending || time_us_32() - int_at_us < INT_SETTLE_US) {
        return false;
    }
    int_pending = false;
    return true;
}

static bool present;
static uint16_t res_x, res_y;
static uint32_t boot_time;
static uint32_t errors;

static bool wr(const uint8_t *b, size_t n) {
    int r = i2c_write_timeout_us(PHOTON_TOUCH_I2C, PHOTON_TOUCH_ADDR, b, n, false, I2C_TIMEOUT_US);
    return r == (int)n;
}

static bool reg_write(uint16_t reg, int value) {
    uint8_t b[3] = { (uint8_t)(reg >> 8), (uint8_t)reg, (uint8_t)value };
    return wr(b, value < 0 ? 2 : 3);
}

// Address phase then a separate read, with a stop in between: the same bus
// sequence as the CircuitPython driver that is known to work on this panel.
static bool reg_read(uint16_t reg, uint8_t *out, size_t n) {
    if (!reg_write(reg, -1)) {
        return false;
    }
    int r = i2c_read_timeout_us(PHOTON_TOUCH_I2C, PHOTON_TOUCH_ADDR, out, n, false, I2C_TIMEOUT_US);
    return r == (int)n;
}

bool touch_init(void) {
    i2c_init(PHOTON_TOUCH_I2C, PHOTON_TOUCH_I2C_HZ);
    gpio_set_function(PHOTON_TOUCH_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PHOTON_TOUCH_SCL, GPIO_FUNC_I2C);
    gpio_init(PHOTON_TOUCH_INT);
    gpio_set_dir(PHOTON_TOUCH_INT, GPIO_IN);
    gpio_pull_up(PHOTON_TOUCH_INT);
    gpio_init(PHOTON_TOUCH_RST);
    gpio_set_dir(PHOTON_TOUCH_RST, GPIO_OUT);

    gpio_set_irq_enabled_with_callback(PHOTON_TOUCH_INT, GPIO_IRQ_EDGE_FALL, true, int_irq);

    gpio_put(PHOTON_TOUCH_RST, 0);
    sleep_ms(2);
    gpio_put(PHOTON_TOUCH_RST, 1);
    sleep_ms(60);

    uint8_t info[24] = { 0 };
    present = reg_write(REG_DEBUG_INFO, -1) && reg_read(REG_INFO_TP_NTX, info, sizeof info);
    reg_write(REG_NORMAL_MODE, -1);
    if (present) {
        // info[] starts at 0xD1F4: resolution at +4, boot time at +8, and
        // the 0xCACA signature in the two bytes at 0xD1FE.
        res_x = (uint16_t)(info[4] | (info[5] << 8));
        res_y = (uint16_t)(info[6] | (info[7] << 8));
        boot_time = (uint32_t)(info[8] | (info[9] << 8) | (info[10] << 16) | ((uint32_t)info[11] << 24));
        present = ((info[11] << 8) | info[10]) == 0xCACA;
    }
    return present;
}

bool touch_present(void) { return present; }
uint32_t touch_errors(void) { return errors; }

void touch_info(uint16_t *rx, uint16_t *ry, uint32_t *bt) {
    *rx = res_x;
    *ry = res_y;
    *bt = boot_time;
}

int touch_read(touch_point_t *pts, int max) {
    uint8_t cnt;
    if (!reg_read(REG_TOUCH_COUNT, &cnt, 1)) {
        errors++;
        return -1;
    }
    int n = cnt & 0x0F;
    if (n == 0 || n > TOUCH_MAX_POINTS) {
        reg_write(REG_TOUCH_COUNT, 0);
        return 0;
    }
    uint8_t b[27];
    if (!reg_read(REG_TOUCH_DATA, b, sizeof b)) {
        errors++;
        return -1;
    }
    reg_write(REG_TOUCH_COUNT, 0);
    if (n > max) {
        n = max;
    }
    for (int i = 0; i < n; i++) {
        // Point 0 at 0xD000, points 1-4 at 0xD007 + 5*(i-1): a 2-byte gap
        // (count/status) follows the first record.
        int o = i * 5 + (i > 0 ? 2 : 0);
        uint16_t x = (uint16_t)((b[o + 1] << 4) | (b[o + 3] >> 4));
        uint16_t y = (uint16_t)((b[o + 2] << 4) | (b[o + 3] & 0x0F));
        pts[i].x = x < PHOTON_LCD_WIDTH ? x : PHOTON_LCD_WIDTH - 1;
        pts[i].y = y < PHOTON_LCD_HEIGHT ? y : PHOTON_LCD_HEIGHT - 1;
        pts[i].strength = b[o + 4];
    }
    return n;
}

// Last poll, kept for callers (so they never read the chip themselves
// and steals frames from LVGL).
static touch_point_t last_pts[TOUCH_MAX_POINTS];
static int last_n;

int touch_last(touch_point_t *pts, int max) {
    int n = last_n < max ? last_n : max;
    for (int i = 0; i < n; i++) {
        pts[i] = last_pts[i];
    }
    return n;
}

// Lift detection is time-based. Measured on this panel (2026-09-30 trace):
// the CST328 reports a held finger every ~26 ms (p90 33 ms), and a read
// between reports sees zero fingers because the previous one was consumed.
// Gaps between *seen* reports reached 72 ms when an early read missed a
// frame. A 30 ms window released and re-pressed a held finger, which
// toggled a stop twice ("animates, then goes back"). 80 ms clears the worst
// gap; actions fire on press, so only release-type buttons wait for it.
#define RELEASE_MS 80

void touch_lvgl_read(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    static touch_point_t held;
    static bool pressed;
    static uint32_t seen_ms;
    static int empty;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    touch_point_t p[TOUCH_MAX_POINTS];
    int n = present ? touch_read(p, TOUCH_MAX_POINTS) : 0;
    static int prev_n;
    if (n != 0 || prev_n != 0) {
        touch_dbg_push(TDBG_POLL, (int16_t)n, n > 0 ? (int16_t)p[0].x : -1, n > 0 ? (int16_t)p[0].y : -1);
    }
    prev_n = n;
    if (n > 0) {
        held = p[0];
        for (int i = 0; i < n; i++) {
            last_pts[i] = p[i];
        }
        last_n = n;
        pressed = true;
        seen_ms = now;
        empty = 0;
    } else if (pressed) {
        empty++;
        if (empty >= 2 && now - seen_ms >= RELEASE_MS) {
            pressed = false;
            last_n = 0;
        }
    }
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    static int prev_state = -1;
    if ((int)data->state != prev_state) {
        touch_dbg_push(TDBG_STATE, (int16_t)data->state, (int16_t)held.x, (int16_t)held.y);
        prev_state = (int)data->state;
    }
    data->point.x = held.x;
    data->point.y = held.y;
}
