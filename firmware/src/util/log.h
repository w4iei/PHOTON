// Console logging over the TinyUSB CDC interface. Text-only diagnostic
// channel; the wire prefix conventions (`# LOG`, `# NOTE`, `BEGIN_TRACE`)
// match the CircuitPython builds so existing host tools keep working.
// Never called from core 1 (core 1 communicates via ipc/rings only).
#ifndef PHOTON_LOG_H
#define PHOTON_LOG_H

#include <stdarg.h>
#include <stdbool.h>

#include <stddef.h>
#include <stdint.h>

// printf to CDC if a terminal is connected; silently dropped otherwise.
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  // "# LOG " prefix
void log_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  // "# NOTE " prefix

// Complete write with USB-task pumping (50 ms bound); used by all log paths.
void log_cdc_write_all(const uint8_t *data, size_t len);

bool log_console_connected(void);

#endif
