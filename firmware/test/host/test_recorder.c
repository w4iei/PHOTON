// microSD recorder tests: the production recorder.c + smf.c + FatFs run
// against a 64 MB RAM disk with a fake millisecond clock. Checks directory
// and file numbering across simulated power cycles, the lazy directory
// create, flush-leaves-a-valid-file, the 30 s silence close and the
// held-note cap, ring overflow accounting, card errors -> remount, no card
// at boot -> late insert, the 9999 stop, and SETUP.TXT once per directory.
// Each suite runs on FAT16, FAT32 and exFAT images.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge/recorder.h"
#include "bridge/smf.h"
#include "ff.h"
#include "diskio.h"

static int checks = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

// ---------------------------------------------------------------------------
// RAM disk behind FatFs diskio
// ---------------------------------------------------------------------------

#define NSECT (64u * 1024u * 1024u / 512u)
static uint8_t disk[NSECT * 512u];
static bool card_present = true;
static bool write_fail = false;
static uint32_t sectors_written;

DSTATUS disk_status(BYTE pdrv) { return (pdrv == 0 && card_present) ? 0 : STA_NOINIT; }
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !card_present) return RES_NOTRDY;
    if (sector + count > NSECT) return RES_PARERR;
    memcpy(buff, disk + sector * 512u, count * 512u);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !card_present) return RES_NOTRDY;
    if (write_fail) return RES_ERROR;
    if (sector + count > NSECT) return RES_PARERR;
    memcpy(disk + sector * 512u, buff, count * 512u);
    sectors_written += count;
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0) return RES_PARERR;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)buff = NSECT; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buff = 1; return RES_OK;
    default: return RES_PARERR;
    }
}

static void format_disk(BYTE fmt) {
    static BYTE work[4096];
    memset(disk, 0, sizeof disk);
    MKFS_PARM parm = { .fmt = (BYTE)(fmt | FM_SFD), .n_fat = 1, .align = 0, .n_root = 0, .au_size = 0 };
    CHECK(f_mkfs("", &parm, work, sizeof work) == FR_OK);
}

// ---------------------------------------------------------------------------
// SMF reader for verification
// ---------------------------------------------------------------------------

typedef struct { uint32_t delta; uint8_t status, d1, d2; } ev_t;

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t read_varlen(const uint8_t *b, size_t *p, size_t len) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        CHECK(*p < len);
        uint8_t c = b[(*p)++];
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80)) return v;
    }
    CHECK(!"varlen too long");
    return 0;
}

// Validates the whole file structure; returns channel events, copies the
// first text meta into `text`.
static int parse_smf(const uint8_t *b, size_t len, ev_t *evs, int cap, char *text, size_t textcap) {
    CHECK(len >= SMF_TRACK_DATA_OFFSET + SMF_EOT_LEN);
    CHECK(memcmp(b, "MThd", 4) == 0 && be32(b + 4) == 6);
    CHECK(b[8] == 0 && b[9] == 0 && b[10] == 0 && b[11] == 1);
    CHECK(b[12] == 0xE7 && b[13] == 0x28);
    CHECK(memcmp(b + 14, "MTrk", 4) == 0);
    CHECK(be32(b + 18) == len - SMF_TRACK_DATA_OFFSET);
    size_t p = SMF_TRACK_DATA_OFFSET;
    int n = 0;
    bool eot = false, track_name = false;
    if (text) text[0] = 0;
    while (p < len && !eot) {
        uint32_t delta = read_varlen(b, &p, len);
        uint8_t st = b[p++];
        CHECK(st & 0x80);  // no running status in these files
        if (st == 0xFF) {
            uint8_t type = b[p++];
            uint32_t l = read_varlen(b, &p, len);
            CHECK(p + l <= len);
            if (type == 0x2F) { CHECK(l == 0 && delta == 0); eot = true; }
            if (type == SMF_META_TRACK_NAME) { CHECK(l == 6 && memcmp(b + p, "PHOTON", 6) == 0); track_name = true; }
            if (type == SMF_META_TEXT && text && text[0] == 0 && l < textcap) { memcpy(text, b + p, l); text[l] = 0; }
            p += l;
        } else if ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0) {
            p += 1;
        } else {
            CHECK(p + 2 <= len);
            if (n < cap) evs[n] = (ev_t){ delta, st, b[p], b[p + 1] };
            n++;
            p += 2;
        }
    }
    CHECK(eot && p == len && track_name);
    return n;
}

static uint8_t filebuf[256 * 1024];

static size_t read_file(const char *path) {
    FIL f;
    UINT br = 0;
    CHECK(f_open(&f, path, FA_READ) == FR_OK);
    CHECK(f_read(&f, filebuf, sizeof filebuf, &br) == FR_OK);
    f_close(&f);
    return br;
}

static bool exists(const char *path, bool want_dir) {
    FILINFO fno;
    if (f_stat(path, &fno) != FR_OK) return false;
    return want_dir == ((fno.fattrib & AM_DIR) != 0);
}

static int count_channel_events(const char *path, ev_t *evs, int cap, char *text) {
    size_t len = read_file(path);
    return parse_smf(filebuf, len, evs, cap, text, 64);
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

static ev_t evs[2048];
static char text[64];

static void boot(uint32_t t) {
    recorder_init();
    recorder_poll(t);
}

static void run_suite(BYTE fmt, uint8_t expect_fs) {
    format_disk(fmt);
    card_present = true;
    write_fail = false;

    // Boot on an empty card: mounted, nothing created.
    boot(0);
    CHECK(g_recorder.state == REC_STATE_IDLE);
    CHECK(g_recorder.fs_type == expect_fs);
    CHECK(g_recorder.next_dir == 1 && g_recorder.dir_num == 0);
    CHECK(g_recorder.card_mb >= 60 && g_recorder.free_mb <= g_recorder.card_mb);
    CHECK(!exists("0001", true));
    recorder_poll(20000);
    CHECK(!exists("0001", true));  // idle boots leave no trace

    // First note: directory and file appear; flush after 500 ms.
    CHECK(recorder_push(1000, 0x91, 60, 100));
    recorder_poll(1000);
    CHECK(g_recorder.state == REC_STATE_RECORDING);
    CHECK(g_recorder.dir_num == 1 && g_recorder.file_num == 1);
    CHECK(exists("0001", true) && exists("0001/0001.MID", false));
    recorder_poll(1499);
    {   // flush cadence runs from the open: nothing on disk yet (size 0)
        FILINFO fno;
        CHECK(f_stat("0001/0001.MID", &fno) == FR_OK && fno.fsize == 0);
    }
    CHECK(recorder_push(1500, 0x81, 60, 64));
    recorder_poll(1500);  // 500 ms after open: first flush, both events
    int n = count_channel_events("0001/0001.MID", evs, 2048, text);
    CHECK(n == 2);
    CHECK(evs[0].delta == 0 && evs[0].status == 0x91 && evs[0].d1 == 60 && evs[0].d2 == 100);
    CHECK(evs[1].delta == 500 && evs[1].status == 0x81 && evs[1].d1 == 60 && evs[1].d2 == 64);
    CHECK(strcmp(text, "PHOTON power-on +00:00:01.000") == 0);

    // Silence close at exactly 30 s after the last event.
    recorder_poll(1500 + 29999);
    CHECK(g_recorder.state == REC_STATE_RECORDING);
    recorder_poll(1500 + 30000);
    CHECK(g_recorder.state == REC_STATE_IDLE && g_recorder.files_closed == 1);
    CHECK(count_channel_events("0001/0001.MID", evs, 2048, NULL) == 2);

    // Second episode in the same power-on: next file number, same directory.
    CHECK(recorder_push(40000, 0x90, 62, 80));
    recorder_poll(40000);
    CHECK(g_recorder.state == REC_STATE_RECORDING && g_recorder.file_num == 2);
    CHECK(exists("0001/0002.MID", false) && !exists("0002", true));
    CHECK(recorder_push(40100, 0x80, 62, 0));
    recorder_poll(40100);
    recorder_poll(70100);
    CHECK(g_recorder.state == REC_STATE_IDLE && g_recorder.files_closed == 2);
    n = count_channel_events("0001/0002.MID", evs, 2048, text);
    CHECK(n == 2 && evs[1].delta == 100);
    CHECK(strcmp(text, "PHOTON power-on +00:00:40.000") == 0);

    // Held note: no close at 30 s, close at the 300 s cap.
    CHECK(recorder_push(80000, 0x90, 64, 90));
    recorder_poll(80000);
    recorder_poll(80000 + 31000);
    CHECK(g_recorder.state == REC_STATE_RECORDING);
    recorder_poll(80000 + 299999);
    CHECK(g_recorder.state == REC_STATE_RECORDING);
    recorder_poll(80000 + 300000);
    CHECK(g_recorder.state == REC_STATE_IDLE && g_recorder.file_num == 3);

    // Burst bigger than the pending buffer: mid-episode flushes, all kept.
    uint32_t t0 = 500000;
    for (int i = 0; i < 600; i++) {
        CHECK(recorder_push(t0 + (uint32_t)i * 7u, (uint8_t)((i & 1) ? 0x80 : 0x90), (uint8_t)(36 + (i / 2) % 48), 77));
        if (i % 50 == 49) recorder_poll(t0 + (uint32_t)i * 7u);  // ring is 512 deep
    }
    recorder_poll(t0 + 600 * 7);
    recorder_poll(t0 + 600 * 7 + 30000);
    CHECK(g_recorder.state == REC_STATE_IDLE && g_recorder.file_num == 4);
    n = count_channel_events("0001/0004.MID", evs, 2048, NULL);
    CHECK(n == 600);
    uint32_t sum = 0;
    for (int i = 0; i < n; i++) sum += evs[i].delta;
    CHECK(sum == 599 * 7);
    CHECK(g_recorder.ring_drops == 0);

    // Ring overflow without a consumer: drops counted, the rest recorded.
    t0 = 1000000;
    for (int i = 0; i < 600; i++) {
        recorder_push(t0 + (uint32_t)i, 0x90, 60, 1);
    }
    CHECK(g_recorder.ring_drops == 600 - 512);
    recorder_poll(t0 + 600);
    recorder_poll(t0 + 600 + 30000);
    // 512 note-ons on the same note: 511 duplicates, still "held" until the cap.
    CHECK(g_recorder.state == REC_STATE_RECORDING);
    recorder_poll(t0 + 600 + 300000);
    CHECK(g_recorder.state == REC_STATE_IDLE);
    CHECK(count_channel_events("0001/0005.MID", evs, 2048, NULL) == 512);

    // Power cycle: the next directory continues from what is on the card.
    boot(0);
    CHECK(g_recorder.state == REC_STATE_IDLE && g_recorder.next_dir == 2);
    CHECK(recorder_push(10, 0x90, 60, 100));
    recorder_poll(10);
    CHECK(exists("0002/0001.MID", false));
    CHECK(recorder_push(20, 0x80, 60, 0));
    recorder_poll(20);
    recorder_poll(30020);
    CHECK(g_recorder.files_closed == 1);

    // Card error mid-episode -> remount -> a fresh directory for what follows.
    CHECK(recorder_push(50000, 0x90, 61, 100));
    recorder_poll(50000);
    CHECK(g_recorder.state == REC_STATE_RECORDING && g_recorder.file_num == 2);
    write_fail = true;
    CHECK(recorder_push(50100, 0x80, 61, 0));
    recorder_poll(50100);
    recorder_poll(50600);
    CHECK(g_recorder.state == REC_STATE_NO_CARD && g_recorder.errors == 1);
    CHECK(strcmp(g_recorder.last_error, "write failed") == 0);
    write_fail = false;
    recorder_poll(50600 + 1999);
    CHECK(g_recorder.state == REC_STATE_NO_CARD);
    recorder_poll(50600 + 2000);
    CHECK(g_recorder.state == REC_STATE_IDLE && g_recorder.next_dir == 3);
    CHECK(recorder_push(60000, 0x90, 60, 100));
    recorder_poll(60000);
    CHECK(exists("0003/0001.MID", false));
    CHECK(recorder_push(60010, 0x80, 60, 0));
    recorder_poll(60010);
    recorder_poll(90010);
    CHECK(g_recorder.state == REC_STATE_IDLE);
    CHECK(count_channel_events("0003/0001.MID", evs, 2048, NULL) == 2);

    // No card at boot: events are dropped, a later insert mounts cleanly.
    card_present = false;
    boot(0);
    CHECK(g_recorder.state == REC_STATE_NO_CARD);
    CHECK(strcmp(g_recorder.last_error, "no card") == 0);
    recorder_push(100, 0x90, 60, 100);
    recorder_poll(100);
    card_present = true;
    recorder_poll(1999);
    CHECK(g_recorder.state == REC_STATE_NO_CARD);
    recorder_poll(2000);
    CHECK(g_recorder.state == REC_STATE_IDLE && g_recorder.next_dir == 4);
    CHECK(g_recorder.last_error == NULL);
    recorder_poll(3000);
    CHECK(!exists("0004", true));  // the pre-mount note was dropped, not replayed

    // Directory 9999 on the card: stop, never wrap.
    CHECK(f_mkdir("9999") == FR_OK);
    boot(0);
    CHECK(g_recorder.state == REC_STATE_STOPPED);
    CHECK(strstr(g_recorder.last_error, "9999") != NULL);
    recorder_push(100, 0x90, 60, 100);
    recorder_poll(100);
    recorder_poll(200);
    CHECK(g_recorder.state == REC_STATE_STOPPED && g_recorder.events_written == 0);
    CHECK(f_unlink("9999") == FR_OK);

    // Non-numeric and foreign entries in the root are ignored by the scan.
    CHECK(f_mkdir("Spotlight-V100") == FR_OK);
    CHECK(f_mkdir("12345") == FR_OK);
    CHECK(f_mkdir("00A1") == FR_OK);
    boot(0);
    CHECK(g_recorder.state == REC_STATE_IDLE && g_recorder.next_dir == 4);

    recorder_init();  // unmount cleanly before the next format
}

// SETUP.TXT: written once per directory, whichever comes first, the text or
// the directory; again in the fresh directory after a remount; a failed
// write leaves the recording alone and is not retried.
static const char SETUP_A[] = "PHOTON setup A\nline two\n";

static bool file_is(const char *path, const char *want) {
    size_t len = read_file(path);
    return len == strlen(want) && memcmp(filebuf, want, len) == 0;
}

static void run_setup_suite(BYTE fmt) {
    format_disk(fmt);
    card_present = true;
    write_fail = false;

    // Text first: nothing on the card until the first note makes the directory.
    boot(0);
    recorder_set_setup(SETUP_A, (uint32_t)strlen(SETUP_A));
    recorder_poll(3000);
    CHECK(!exists("0001", true) && g_recorder.setup_dir == 0);
    CHECK(recorder_push(5000, 0x90, 60, 100));
    recorder_poll(5000);
    CHECK(g_recorder.state == REC_STATE_RECORDING);
    CHECK(g_recorder.setup_dir == 1 && g_recorder.setup_ok);
    CHECK(file_is("0001/SETUP.TXT", SETUP_A));
    // Once: removed, it is not written again in this directory.
    CHECK(f_unlink("0001/SETUP.TXT") == FR_OK);
    CHECK(recorder_push(5100, 0x80, 60, 0));
    recorder_poll(5100);
    recorder_poll(35100);
    CHECK(g_recorder.state == REC_STATE_IDLE);
    CHECK(recorder_push(40000, 0x90, 62, 90));
    recorder_poll(40000);
    CHECK(exists("0001/0002.MID", false) && !exists("0001/SETUP.TXT", false));
    CHECK(recorder_push(40100, 0x80, 62, 0));
    recorder_poll(40100);
    recorder_poll(70100);
    CHECK(count_channel_events("0001/0002.MID", evs, 2048, NULL) == 2);

    // Directory first (a note right after power-on), text while recording.
    boot(0);
    CHECK(recorder_push(200, 0x90, 60, 100));
    recorder_poll(200);
    CHECK(g_recorder.dir_num == 2 && !exists("0002/SETUP.TXT", false));
    recorder_poll(1000);
    CHECK(!exists("0002/SETUP.TXT", false));
    recorder_set_setup(SETUP_A, (uint32_t)strlen(SETUP_A));
    CHECK(recorder_push(3000, 0x80, 60, 0));
    recorder_poll(3000);
    CHECK(g_recorder.state == REC_STATE_RECORDING && g_recorder.setup_dir == 2);
    CHECK(file_is("0002/SETUP.TXT", SETUP_A));
    recorder_poll(33000);
    CHECK(g_recorder.state == REC_STATE_IDLE);
    CHECK(count_channel_events("0002/0001.MID", evs, 2048, NULL) == 2);

    // Text after the first episode closed: written while idle.
    boot(0);
    CHECK(recorder_push(200, 0x90, 60, 100));
    recorder_poll(200);
    CHECK(recorder_push(300, 0x80, 60, 0));
    recorder_poll(300);
    recorder_poll(30300);
    CHECK(g_recorder.state == REC_STATE_IDLE && g_recorder.dir_num == 3);
    recorder_set_setup(SETUP_A, (uint32_t)strlen(SETUP_A));
    recorder_poll(31000);
    CHECK(g_recorder.setup_dir == 3 && file_is("0003/SETUP.TXT", SETUP_A));

    // Card error -> remount -> fresh directory gets its own copy.
    CHECK(recorder_push(40000, 0x90, 61, 100));
    recorder_poll(40000);
    write_fail = true;
    CHECK(recorder_push(40100, 0x80, 61, 0));
    recorder_poll(40100);
    recorder_poll(40600);
    CHECK(g_recorder.state == REC_STATE_NO_CARD && g_recorder.setup_dir == 3);
    write_fail = false;
    recorder_poll(42600);
    CHECK(g_recorder.state == REC_STATE_IDLE && g_recorder.next_dir == 4);
    CHECK(g_recorder.setup_dir == 0);
    recorder_poll(43000);
    CHECK(!exists("0004", true));  // still no note, still no directory
    CHECK(recorder_push(50000, 0x90, 60, 100));
    recorder_poll(50000);
    CHECK(g_recorder.setup_dir == 4 && file_is("0004/SETUP.TXT", SETUP_A));
    CHECK(recorder_push(50100, 0x80, 60, 0));
    recorder_poll(50100);
    recorder_poll(80100);
    CHECK(g_recorder.state == REC_STATE_IDLE);

    // A failed SETUP.TXT: reported, not retried, recording goes on.
    boot(0);
    CHECK(recorder_push(200, 0x90, 60, 100));
    recorder_poll(200);
    CHECK(recorder_push(300, 0x80, 60, 0));
    recorder_poll(300);
    recorder_poll(30300);
    CHECK(g_recorder.state == REC_STATE_IDLE && g_recorder.dir_num == 5);
    uint32_t errors = g_recorder.errors;
    write_fail = true;
    recorder_set_setup(SETUP_A, (uint32_t)strlen(SETUP_A));
    recorder_poll(31000);
    CHECK(g_recorder.setup_dir == 5 && !g_recorder.setup_ok);
    CHECK(g_recorder.state == REC_STATE_IDLE && g_recorder.errors == errors);
    write_fail = false;
    recorder_poll(32000);
    CHECK(g_recorder.setup_dir == 5 && !g_recorder.setup_ok);
    CHECK(recorder_push(40000, 0x90, 64, 100));
    recorder_poll(40000);
    CHECK(recorder_push(40100, 0x80, 64, 0));
    recorder_poll(40100);
    recorder_poll(70100);
    CHECK(g_recorder.state == REC_STATE_IDLE && g_recorder.file_num == 2);
    CHECK(count_channel_events("0005/0002.MID", evs, 2048, NULL) == 2);

    // No text at all (no bridge settings yet): no SETUP.TXT, recording as before.
    boot(0);
    CHECK(recorder_push(200, 0x90, 60, 100));
    recorder_poll(200);
    recorder_poll(5000);
    CHECK(g_recorder.dir_num == 6 && !exists("0006/SETUP.TXT", false));
    CHECK(g_recorder.setup_dir == 0);

    recorder_init();
}

static void test_smf_varlen(void) {
    uint8_t b[4];
    CHECK(smf_put_varlen(b, 0) == 1 && b[0] == 0x00);
    CHECK(smf_put_varlen(b, 0x7F) == 1 && b[0] == 0x7F);
    CHECK(smf_put_varlen(b, 0x80) == 2 && b[0] == 0x81 && b[1] == 0x00);
    CHECK(smf_put_varlen(b, 0x3FFF) == 2 && b[0] == 0xFF && b[1] == 0x7F);
    CHECK(smf_put_varlen(b, 0x4000) == 3 && b[0] == 0x81 && b[1] == 0x80 && b[2] == 0x00);
    CHECK(smf_put_varlen(b, 0x0FFFFFFF) == 4 && b[0] == 0xFF && b[3] == 0x7F);
    CHECK(smf_put_varlen(b, 0xFFFFFFFF) == 4 && b[0] == 0xFF && b[3] == 0x7F);  // clamped
}

int main(void) {
    test_smf_varlen();
    run_suite(FM_FAT, FS_FAT16);
    run_suite(FM_FAT32, FS_FAT32);
    run_suite(FM_EXFAT, FS_EXFAT);
    run_setup_suite(FM_FAT);
    run_setup_suite(FM_FAT32);
    run_setup_suite(FM_EXFAT);
    printf("test_recorder: %d checks passed\n", checks);
    return 0;
}
