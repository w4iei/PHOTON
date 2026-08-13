// Frame format v2 (shared wire format with the future RS-422 mesh):
//
//   A5 5A | type | flags | src | dst | len | seq_lo seq_hi | hdr_crc8
//         | payload[len] | crc32 (LE)
//
// hdr_crc8: poly 0x07 over type..seq (7 bytes) — rejects false SOF within
// 8 bytes and makes resynchronization O(n).
// crc32: reflected IEEE over type..payload (8 header bytes + payload) —
// unlike v1 it covers the header, so corrupted len/type/addresses cannot
// pass.
#ifndef PHOTON_FRAME_H
#define PHOTON_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "board_config.h"

// Frame types (ASCII like v1 where an equivalent existed)
#define PHOTON_FT_EVT_POLL   'Q'
#define PHOTON_FT_EVT_BATCH  'E'
#define PHOTON_FT_PING       'P'
#define PHOTON_FT_PONG       'O'
#define PHOTON_FT_DATA_REQ   'R'
#define PHOTON_FT_DATA_RESP  'D'
#define PHOTON_FT_MINMAX_REQ 'M'
#define PHOTON_FT_MINMAX_RESP 'm'
#define PHOTON_FT_STATS_REQ  'S'
#define PHOTON_FT_STATS_RESP 's'
#define PHOTON_FT_TRACE_START 'T'
#define PHOTON_FT_TRACE_DATA 't'
#define PHOTON_FT_CAL_SET    'K'
#define PHOTON_FT_CAL_COMMIT 'C'
#define PHOTON_FT_CAL_ACK    'k'
#define PHOTON_FT_TEST_BURST 'B'

// flags
#define PHOTON_FLAG_DIR        (1u << 0)  // reserved for RS-422 routing
#define PHOTON_FLAG_PRIO_EVENT (1u << 1)
#define PHOTON_FLAG_VERSION    (2u << 4)
#define PHOTON_FLAG_VER_MASK   (0xFu << 4)

#define PHOTON_ADDR_BRIDGE     0x00
#define PHOTON_ADDR_BROADCAST  0xFF

// CAL_SET payload[0] carrying this index means "reset all sensors" —
// shared by the console sender and the node handler.
#define PHOTON_CAL_IDX_ALL     0xFF

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint8_t src;
    uint8_t dst;
    uint8_t len;
    uint16_t seq;
    uint8_t payload[PHOTON_FRAME_MAX_PAYLOAD];
} photon_frame_t;

// Encode into out (>= PHOTON_FRAME_MAX_LEN); returns wire length.
size_t frame_encode(const photon_frame_t *f, uint8_t *out);

typedef enum {
    FRAME_PARSE_NEED_MORE = 0,  // *consumed bytes of leading garbage may be dropped
    FRAME_PARSE_OK = 1,         // *consumed bytes eaten (garbage + whole frame)
} frame_parse_result_t;

typedef struct {
    uint32_t crc_errors;
    uint32_t hdr_errors;
} frame_parse_stats_t;

// Scan buf[0..len) for the first complete valid frame. O(len) worst case.
frame_parse_result_t frame_parse(const uint8_t *buf, size_t len,
                                 size_t *consumed, photon_frame_t *out,
                                 frame_parse_stats_t *stats);

#endif
