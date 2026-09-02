#include "bridge/smf.h"

#include <string.h>

void smf_put_be32(uint8_t out[4], uint32_t v) {
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)v;
}

size_t smf_put_header(uint8_t *out) {
    memcpy(out, "MThd", 4);
    smf_put_be32(out + 4, 6);
    out[8] = 0;  // format 0
    out[9] = 0;
    out[10] = 0; // one track
    out[11] = 1;
    out[12] = (uint8_t)(SMF_DIVISION_1MS >> 8);
    out[13] = (uint8_t)SMF_DIVISION_1MS;
    return SMF_HEADER_LEN;
}

size_t smf_put_track_header(uint8_t *out, uint32_t len) {
    memcpy(out, "MTrk", 4);
    smf_put_be32(out + 4, len);
    return SMF_TRACK_HEADER_LEN;
}

size_t smf_put_varlen(uint8_t *out, uint32_t v) {
    if (v > SMF_MAX_DELTA) {
        v = SMF_MAX_DELTA;
    }
    uint8_t tmp[4];
    int n = 0;
    do {
        tmp[n++] = (uint8_t)(v & 0x7F);
        v >>= 7;
    } while (v);
    size_t written = 0;
    while (n--) {
        out[written++] = (uint8_t)(tmp[n] | (n ? 0x80 : 0));
    }
    return written;
}

size_t smf_put_event(uint8_t *out, uint32_t delta, uint8_t status, uint8_t d1, uint8_t d2) {
    size_t n = smf_put_varlen(out, delta);
    out[n++] = status;
    out[n++] = (uint8_t)(d1 & 0x7F);
    out[n++] = (uint8_t)(d2 & 0x7F);
    return n;
}

size_t smf_put_meta(uint8_t *out, uint32_t delta, uint8_t type, const void *data, size_t len) {
    size_t n = smf_put_varlen(out, delta);
    out[n++] = 0xFF;
    out[n++] = type;
    n += smf_put_varlen(out + n, (uint32_t)len);
    memcpy(out + n, data, len);
    return n + len;
}

size_t smf_put_end_of_track(uint8_t *out) {
    out[0] = 0x00;
    out[1] = 0xFF;
    out[2] = 0x2F;
    out[3] = 0x00;
    return SMF_EOT_LEN;
}
