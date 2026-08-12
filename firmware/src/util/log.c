#include "util/log.h"

#include <stdio.h>
#include <string.h>

#include "tusb.h"

bool log_console_connected(void) {
    return tud_cdc_connected();
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
    // Best-effort: drop rather than block if the host isn't draining.
    tud_cdc_write(buf, (uint32_t)n);
    tud_cdc_write_flush();
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
