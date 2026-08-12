// Frame v2 codec tests: CRC vectors, roundtrip, corruption rejection, and a
// streaming fuzz that embeds frames in noise and requires 100% recovery.
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comms/frame.h"
#include "util/crc32.h"

static int checks = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

static void test_crc_vectors(void) {
    // Standard check values.
    CHECK(photon_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
    CHECK(photon_crc8((const uint8_t *)"123456789", 9) == 0xF4);
    // v1 compatibility: CRC32 of empty payload.
    CHECK(photon_crc32((const uint8_t *)"", 0) == 0x00000000u);
}

static photon_frame_t mk_frame(uint8_t type, uint8_t src, uint8_t dst,
                               uint16_t seq, uint8_t len) {
    photon_frame_t f = { 0 };
    f.type = type;
    f.src = src;
    f.dst = dst;
    f.seq = seq;
    f.len = len;
    for (int i = 0; i < len; i++) {
        f.payload[i] = (uint8_t)(rand() & 0xFF);
    }
    return f;
}

static void test_roundtrip(void) {
    for (int len = 0; len <= PHOTON_FRAME_MAX_PAYLOAD; len++) {
        photon_frame_t f = mk_frame('E', 1, 0, (uint16_t)(len * 7), (uint8_t)len);
        uint8_t wire[PHOTON_FRAME_MAX_LEN];
        size_t n = frame_encode(&f, wire);
        CHECK(n == (size_t)(PHOTON_FRAME_HEADER_LEN + len + PHOTON_FRAME_CRC_LEN));
        photon_frame_t out;
        size_t consumed = 0;
        CHECK(frame_parse(wire, n, &consumed, &out, NULL) == FRAME_PARSE_OK);
        CHECK(consumed == n);
        CHECK(out.type == f.type && out.src == f.src && out.dst == f.dst);
        CHECK(out.seq == f.seq && out.len == f.len);
        CHECK(memcmp(out.payload, f.payload, len) == 0);
        CHECK((out.flags & PHOTON_FLAG_VER_MASK) == PHOTON_FLAG_VERSION);
    }
}

static void test_corruption(void) {
    photon_frame_t f = mk_frame('D', 2, 0, 999, 64);
    uint8_t wire[PHOTON_FRAME_MAX_LEN];
    size_t n = frame_encode(&f, wire);
    // Flip every byte in turn: parser must never deliver corrupted content.
    for (size_t k = 0; k < n; k++) {
        uint8_t mut[PHOTON_FRAME_MAX_LEN];
        memcpy(mut, wire, n);
        mut[k] ^= 0x40;
        photon_frame_t out;
        size_t consumed = 0;
        frame_parse_result_t r = frame_parse(mut, n, &consumed, &out, NULL);
        if (r == FRAME_PARSE_OK) {
            // Only acceptable if content matches the original exactly
            // (impossible here since we flipped a covered byte).
            CHECK(memcmp(out.payload, f.payload, f.len) == 0 && out.seq == f.seq);
            fprintf(stderr, "corrupted byte %zu accepted\n", k);
            exit(1);
        }
    }
}

// Streaming fuzz: frames embedded in random noise, delivered in random-size
// chunks through an assembly buffer exactly like the transport uses.
static void test_stream_fuzz(void) {
    srand(1234);
    enum { N_FRAMES = 500 };
    photon_frame_t sent[N_FRAMES];
    uint8_t stream[N_FRAMES * (PHOTON_FRAME_MAX_LEN + 24)];
    size_t stream_len = 0;

    for (int i = 0; i < N_FRAMES; i++) {
        // Leading noise (may contain A5 5A by chance).
        int noise = rand() % 20;
        for (int k = 0; k < noise; k++) {
            stream[stream_len++] = (uint8_t)(rand() & 0xFF);
        }
        sent[i] = mk_frame((uint8_t)('A' + (i % 20)), (uint8_t)(i % 6 + 1), 0,
                           (uint16_t)i, (uint8_t)(rand() % (PHOTON_FRAME_MAX_PAYLOAD + 1)));
        stream_len += frame_encode(&sent[i], stream + stream_len);
    }

    uint8_t assembly[512];
    size_t assembly_len = 0;
    size_t fed = 0;
    int got = 0;
    frame_parse_stats_t stats = { 0 };

    while (fed < stream_len || assembly_len > 0) {
        size_t chunk = (size_t)(1 + rand() % 63);
        if (chunk > stream_len - fed) {
            chunk = stream_len - fed;
        }
        while (chunk > 0 && assembly_len < sizeof assembly) {
            size_t room = sizeof assembly - assembly_len;
            size_t take = chunk < room ? chunk : room;
            memcpy(assembly + assembly_len, stream + fed, take);
            assembly_len += take;
            fed += take;
            chunk -= take;
        }
        bool progress = true;
        while (progress) {
            progress = false;
            size_t consumed = 0;
            photon_frame_t out;
            frame_parse_result_t r =
                frame_parse(assembly, assembly_len, &consumed, &out, &stats);
            if (consumed > 0) {
                memmove(assembly, assembly + consumed, assembly_len - consumed);
                assembly_len -= consumed;
                progress = true;
            }
            if (r == FRAME_PARSE_OK) {
                CHECK(got < N_FRAMES);
                photon_frame_t *ref = &sent[got];
                CHECK(out.type == ref->type && out.seq == ref->seq &&
                      out.len == ref->len);
                CHECK(memcmp(out.payload, ref->payload, ref->len) == 0);
                got++;
                progress = true;
            }
        }
        if (fed >= stream_len && got == N_FRAMES) {
            break;
        }
    }
    CHECK(got == N_FRAMES);
    printf("stream fuzz: %d/%d frames recovered, %u hdr rejects on noise\n",
           got, N_FRAMES, stats.hdr_errors);
}

int main(void) {
    test_crc_vectors();
    test_roundtrip();
    test_corruption();
    test_stream_fuzz();
    printf("test_frame OK (%d checks)\n", checks);
    return 0;
}
