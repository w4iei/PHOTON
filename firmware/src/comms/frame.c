#include "comms/frame.h"

#include <string.h>

#include "util/crc32.h"

size_t frame_encode(const photon_frame_t *f, uint8_t *out) {
    out[0] = 0xA5;
    out[1] = 0x5A;
    out[2] = f->type;
    out[3] = (uint8_t)((f->flags & ~PHOTON_FLAG_VER_MASK) | PHOTON_FLAG_VERSION);
    out[4] = f->src;
    out[5] = f->dst;
    out[6] = f->len;
    out[7] = (uint8_t)(f->seq & 0xFF);
    out[8] = (uint8_t)(f->seq >> 8);
    out[9] = photon_crc8(&out[2], 7);
    memcpy(&out[10], f->payload, f->len);
    uint32_t crc = photon_crc32(&out[2], 8u + f->len);
    size_t off = 10u + f->len;
    out[off + 0] = (uint8_t)(crc & 0xFF);
    out[off + 1] = (uint8_t)((crc >> 8) & 0xFF);
    out[off + 2] = (uint8_t)((crc >> 16) & 0xFF);
    out[off + 3] = (uint8_t)((crc >> 24) & 0xFF);
    return off + 4;
}

frame_parse_result_t frame_parse(const uint8_t *buf, size_t len,
                                 size_t *consumed, photon_frame_t *out,
                                 frame_parse_stats_t *stats) {
    size_t i = 0;
    while (i + 2 <= len) {
        if (buf[i] != 0xA5 || buf[i + 1] != 0x5A) {
            i++;
            continue;
        }
        // Candidate SOF at i. Header complete?
        if (len - i < PHOTON_FRAME_HEADER_LEN) {
            *consumed = i;  // drop garbage before the candidate, wait for more
            return FRAME_PARSE_NEED_MORE;
        }
        const uint8_t *h = &buf[i];
        if (photon_crc8(&h[2], 7) != h[9]) {
            if (stats) {
                stats->hdr_errors++;
            }
            i++;  // false SOF or corrupt header: resume scan at next byte
            continue;
        }
        uint8_t plen = h[6];
        if (plen > PHOTON_FRAME_MAX_PAYLOAD) {
            if (stats) {
                stats->hdr_errors++;
            }
            i++;
            continue;
        }
        size_t total = PHOTON_FRAME_HEADER_LEN + (size_t)plen + PHOTON_FRAME_CRC_LEN;
        if (len - i < total) {
            *consumed = i;
            return FRAME_PARSE_NEED_MORE;
        }
        const uint8_t *cp = &h[10 + plen];
        uint32_t wire_crc = (uint32_t)cp[0] | ((uint32_t)cp[1] << 8) |
                            ((uint32_t)cp[2] << 16) | ((uint32_t)cp[3] << 24);
        if (photon_crc32(&h[2], 8u + plen) != wire_crc) {
            if (stats) {
                stats->crc_errors++;
            }
            i++;
            continue;
        }
        out->type = h[2];
        out->flags = h[3];
        out->src = h[4];
        out->dst = h[5];
        out->len = plen;
        out->seq = (uint16_t)(h[7] | (h[8] << 8));
        memcpy(out->payload, &h[10], plen);
        if (stats) {
            stats->frames_ok++;
        }
        *consumed = i + total;
        return FRAME_PARSE_OK;
    }
    // No SOF candidate: keep at most the final byte (may be a split 0xA5).
    *consumed = len > 0 ? len - 1 : 0;
    return FRAME_PARSE_NEED_MORE;
}
