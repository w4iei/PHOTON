// Standard MIDI File (format 0) byte encoders. Pure functions over caller
// buffers, no I/O, so the recorder's file layout is host-testable.
//
// Division is SMPTE 25 fps x 40 ticks/frame = exactly 1 ms per tick, which
// sidesteps tempo maps entirely: delta times are milliseconds.
#ifndef PHOTON_SMF_H
#define PHOTON_SMF_H

#include <stddef.h>
#include <stdint.h>

#define SMF_DIVISION_1MS      0xE728u  // -25 fps, 40 ticks/frame
#define SMF_HEADER_LEN        14       // MThd chunk
#define SMF_TRACK_HEADER_LEN  8        // MTrk + length
#define SMF_TRACK_LEN_OFFSET  (SMF_HEADER_LEN + 4)
#define SMF_TRACK_DATA_OFFSET (SMF_HEADER_LEN + SMF_TRACK_HEADER_LEN)
#define SMF_EOT_LEN           4
#define SMF_MAX_DELTA         0x0FFFFFFFu  // 4-byte varlen ceiling (~74 h in ms)
#define SMF_META_TEXT         0x01
#define SMF_META_TRACK_NAME   0x03

// Each returns the number of bytes written.
size_t smf_put_header(uint8_t *out);                        // 14 bytes
size_t smf_put_track_header(uint8_t *out, uint32_t len);    // 8 bytes
void   smf_put_be32(uint8_t out[4], uint32_t v);
size_t smf_put_varlen(uint8_t *out, uint32_t v);            // 1-4 bytes
size_t smf_put_event(uint8_t *out, uint32_t delta,
                     uint8_t status, uint8_t d1, uint8_t d2); // <= 7 bytes
size_t smf_put_meta(uint8_t *out, uint32_t delta, uint8_t type,
                    const void *data, size_t len);           // <= 6 + len
size_t smf_put_end_of_track(uint8_t *out);                  // 4 bytes

#endif
