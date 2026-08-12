#include "util/log.h"

#include <stdio.h>
#include <string.h>

#include "tusb.h"

#include "pico/time.h"

bool log_console_connected(void) {
    return tud_cdc_connected();
}

// Write everything, pumping the USB task while the CDC FIFO drains, so
// multi-line output (sensor tables) arrives complete instead of truncating
// at the FIFO size. Bounded: gives up after 50 ms if the host stalls.
void log_cdc_write_all(const uint8_t *data, size_t len) {
    absolute_time_t deadline = make_timeout_time_ms(50);
    while (len > 0 && tud_cdc_connected()) {
        uint32_t wrote = tud_cdc_write(data, (uint32_t)len);
        tud_cdc_write_flush();
        data += wrote;
        len -= wrote;
        if (wrote == 0) {
            if (time_reached(deadline)) {
                break;
            }
            tud_task();  // main-loop context only; drains the FIFO
        }
    }
}

static void log_write(const char *prefix, const char *fmt, va_list ap) {
    if (!tud_cdc_connected()) {
        return;
    }
    char buf[256];
    int n = 0;
    if (prefix != NULL) {
        n = snprintf(buf, sizeof buf, "%s", prefix);
    }
    n += vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
    if (n > (int)sizeof buf - 3) {
        n = (int)sizeof buf - 3;
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    log_cdc_write_all((const uint8_t *)buf, (size_t)n);
}

void log_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_write(NULL, fmt, ap);
    va_end(ap);
}

void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_write("# LOG ", fmt, ap);
    va_end(ap);
}

void log_note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_write("# NOTE ", fmt, ap);
    va_end(ap);
}
