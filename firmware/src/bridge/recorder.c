#include "bridge/recorder.h"

#include <stdio.h>
#include <string.h>

#include "hardware/sync.h"

#include "bridge/smf.h"
#include "ff.h"

#define REC_RING_SLOTS     512
#define REC_PENDING_BYTES  2048
#define REC_EVENT_MAX      7  // 4-byte varlen + 3-byte message

typedef struct {
    uint32_t t_ms;
    uint8_t status, d1, d2, _pad;
} rec_msg_t;

static struct {
    rec_msg_t slots[REC_RING_SLOTS];
    volatile uint32_t head;  // consumer (core 1)
    volatile uint32_t tail;  // producer (core 0)
} ring;

recorder_status_t g_recorder;

static FATFS fs;
static FIL fil;
static bool mounted, file_open;
static bool mount_tried;
static uint32_t last_mount_try_ms;
static uint32_t next_dir;          // from the root scan at mount time
static uint8_t pending[REC_PENDING_BYTES];
static uint32_t pending_len;
static uint32_t data_end;          // track bytes on disk, excluding EOT
static uint32_t last_flush_ms, last_event_ms, last_t_ms;
static uint32_t held[16][4];       // per channel, 128 note bits
static uint32_t held_count;

// ---------------------------------------------------------------------------
// Ring (core 0 producer, core 1 consumer)
// ---------------------------------------------------------------------------

bool recorder_push(uint32_t t_ms, uint8_t status, uint8_t d1, uint8_t d2) {
    if (!g_recorder.enabled) {
        return false;
    }
    if (ring.tail - ring.head >= REC_RING_SLOTS) {
        g_recorder.ring_drops++;
        return false;
    }
    rec_msg_t *s = &ring.slots[ring.tail % REC_RING_SLOTS];
    s->t_ms = t_ms;
    s->status = status;
    s->d1 = d1;
    s->d2 = d2;
    __dmb();
    ring.tail = ring.tail + 1;
    return true;
}

static bool pop(rec_msg_t *out) {
    if (ring.head == ring.tail) {
        return false;
    }
    __dmb();
    *out = ring.slots[ring.head % REC_RING_SLOTS];
    __dmb();
    ring.head = ring.head + 1;
    return true;
}

static void drain(void) {
    rec_msg_t m;
    while (pop(&m)) {
    }
}

// ---------------------------------------------------------------------------
// Card state
// ---------------------------------------------------------------------------

static void reset_episode(void) {
    pending_len = 0;
    held_count = 0;
    memset(held, 0, sizeof held);
}

// A card error: drop the file, unmount, go back to probing (after the usual
// retry interval, so a glitching card gets a moment before the next attempt).
static void fail(uint32_t now_ms, const char *why) {
    if (file_open) {
        f_close(&fil);
        file_open = false;
    }
    if (mounted) {
        f_unmount("");
        mounted = false;
    }
    reset_episode();
    g_recorder.errors++;
    g_recorder.last_error = why;
    g_recorder.state = REC_STATE_NO_CARD;
    mount_tried = true;
    last_mount_try_ms = now_ms;
}

// A limit: keep the card mounted (status stays readable) but write nothing.
static void stop(const char *why) {
    if (file_open) {
        f_close(&fil);
        file_open = false;
    }
    reset_episode();
    g_recorder.last_error = why;
    g_recorder.state = REC_STATE_STOPPED;
}

static bool parse_number4(const char *name, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        v = v * 10 + (uint32_t)(name[i] - '0');
    }
    if (name[4] != '\0') {
        return false;
    }
    *out = v;
    return true;
}

static void try_mount(uint32_t now_ms) {
    FRESULT fr = f_mount(&fs, "", 1);
    if (fr != FR_OK) {
        g_recorder.last_error = fr == FR_NO_FILESYSTEM ? "no FAT/exFAT filesystem on card"
                                : (fr == FR_NOT_READY || fr == FR_DISK_ERR) ? "no card"
                                                                             : "mount failed";
        return;
    }
    mounted = true;
    g_recorder.fs_type = fs.fs_type;

    // Highest existing NNNN directory decides where this power-on continues.
    DIR dir;
    FILINFO fno;
    uint32_t max = 0;
    if (f_opendir(&dir, "") != FR_OK) {
        fail(now_ms, "root directory unreadable");
        return;
    }
    for (;;) {
        if (f_readdir(&dir, &fno) != FR_OK) {
            f_closedir(&dir);
            fail(now_ms, "root directory unreadable");
            return;
        }
        if (fno.fname[0] == '\0') {
            break;
        }
        uint32_t n;
        if ((fno.fattrib & AM_DIR) && parse_number4(fno.fname, &n) && n > max) {
            max = n;
        }
    }
    f_closedir(&dir);
    next_dir = max + 1;
    g_recorder.next_dir = (uint16_t)(next_dir > 0xFFFF ? 0xFFFF : next_dir);
    g_recorder.dir_num = 0;
    g_recorder.file_num = 0;

    DWORD free_clusters;
    FATFS *pfs;
    if (f_getfree("", &free_clusters, &pfs) == FR_OK) {
        uint64_t total = (uint64_t)(pfs->n_fatent - 2) * pfs->csize * 512u;
        uint64_t avail = (uint64_t)free_clusters * pfs->csize * 512u;
        g_recorder.card_mb = (uint32_t)(total >> 20);
        g_recorder.free_mb = (uint32_t)(avail >> 20);
    }

    g_recorder.last_error = NULL;
    if (next_dir > PHOTON_REC_MAX_NUMBER) {
        stop("directory 9999 already on card");
        return;
    }
    g_recorder.state = REC_STATE_IDLE;
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

// Write everything pending, then re-terminate the file: end-of-track after
// the data and the real track length in the header. The result is a valid
// SMF on the card after every call.
static bool flush(uint32_t now_ms) {
    if (!file_open) {
        return true;
    }
    UINT bw;
    if (f_lseek(&fil, data_end) != FR_OK) {
        fail(now_ms, "seek failed");
        return false;
    }
    if (pending_len) {
        FRESULT fr = f_write(&fil, pending, pending_len, &bw);
        if (fr != FR_OK) {
            fail(now_ms, "write failed");
            return false;
        }
        if (bw != pending_len) {
            stop("card full");
            return false;
        }
        data_end += pending_len;
        g_recorder.bytes_written += pending_len;
        pending_len = 0;
    }
    uint8_t tail[SMF_EOT_LEN];
    smf_put_end_of_track(tail);
    uint8_t len4[4];
    smf_put_be32(len4, data_end + SMF_EOT_LEN - SMF_TRACK_DATA_OFFSET);
    if (f_write(&fil, tail, sizeof tail, &bw) != FR_OK || bw != sizeof tail ||
        f_lseek(&fil, SMF_TRACK_LEN_OFFSET) != FR_OK ||
        f_write(&fil, len4, sizeof len4, &bw) != FR_OK || bw != sizeof len4 ||
        f_sync(&fil) != FR_OK) {
        fail(now_ms, "write failed");
        return false;
    }
    last_flush_ms = now_ms;
    return true;
}

static bool open_file(uint32_t now_ms, uint32_t first_t_ms) {
    char path[16];
    if (g_recorder.dir_num == 0) {
        if (next_dir > PHOTON_REC_MAX_NUMBER) {
            stop("directory 9999 reached");
            return false;
        }
        snprintf(path, sizeof path, "%04lu", (unsigned long)next_dir);
        FRESULT fr = f_mkdir(path);
        if (fr != FR_OK && fr != FR_EXIST) {
            fail(now_ms, "mkdir failed");
            return false;
        }
        g_recorder.dir_num = (uint16_t)next_dir;
        g_recorder.file_num = 0;
    }
    if (g_recorder.file_num >= PHOTON_REC_MAX_NUMBER) {
        stop("file 9999 reached");
        return false;
    }
    g_recorder.file_num++;
    snprintf(path, sizeof path, "%04u/%04u.MID", (unsigned)g_recorder.dir_num,
             (unsigned)g_recorder.file_num);
    if (f_open(&fil, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        fail(now_ms, "file create failed");
        return false;
    }
    file_open = true;
    data_end = 0;
    reset_episode();

    uint8_t *p = pending;
    p += smf_put_header(p);
    p += smf_put_track_header(p, 0);  // patched on every flush
    p += smf_put_meta(p, 0, SMF_META_TRACK_NAME, "PHOTON", 6);
    uint32_t s = first_t_ms / 1000u;
    char text[48];
    int n = snprintf(text, sizeof text, "PHOTON power-on +%02lu:%02lu:%02lu.%03lu",
                     (unsigned long)(s / 3600u), (unsigned long)(s / 60u % 60u),
                     (unsigned long)(s % 60u), (unsigned long)(first_t_ms % 1000u));
    p += smf_put_meta(p, 0, SMF_META_TEXT, text, (size_t)n);
    pending_len = (uint32_t)(p - pending);

    last_t_ms = first_t_ms;
    last_event_ms = first_t_ms;
    last_flush_ms = now_ms;
    g_recorder.state = REC_STATE_RECORDING;
    return true;
}

static void close_file(uint32_t now_ms) {
    if (!flush(now_ms)) {
        return;  // fail()/stop() already moved the state
    }
    f_close(&fil);
    file_open = false;
    reset_episode();
    g_recorder.files_closed++;
    g_recorder.state = REC_STATE_IDLE;
}

static bool handle(uint32_t now_ms, const rec_msg_t *m) {
    if (pending_len + REC_EVENT_MAX > REC_PENDING_BYTES && !flush(now_ms)) {
        return false;
    }
    uint32_t delta = m->t_ms - last_t_ms;
    last_t_ms = m->t_ms;
    last_event_ms = m->t_ms;
    pending_len += (uint32_t)smf_put_event(pending + pending_len, delta, m->status, m->d1, m->d2);
    g_recorder.events_written++;

    uint8_t kind = m->status & 0xF0;
    uint8_t ch = m->status & 0x0F;
    uint8_t note = m->d1 & 0x7F;
    uint32_t bit = 1u << (note & 31);
    uint32_t *word = &held[ch][note >> 5];
    if (kind == 0x90 && m->d2 != 0) {
        if (!(*word & bit)) {
            *word |= bit;
            held_count++;
        }
    } else if (kind == 0x80 || kind == 0x90) {
        if (*word & bit) {
            *word &= ~bit;
            held_count--;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------

void recorder_poll(uint32_t now_ms) {
    rec_msg_t m;
    switch (g_recorder.state) {
    case REC_STATE_NO_CARD:
        drain();
        if (!mount_tried || (int32_t)(now_ms - last_mount_try_ms) >= (int32_t)PHOTON_REC_MOUNT_RETRY_MS) {
            mount_tried = true;
            last_mount_try_ms = now_ms;
            try_mount(now_ms);
        }
        break;

    case REC_STATE_IDLE:
        if (pop(&m) && open_file(now_ms, m.t_ms)) {
            handle(now_ms, &m);
        }
        break;

    case REC_STATE_RECORDING:
        while (g_recorder.state == REC_STATE_RECORDING && pop(&m)) {
            handle(now_ms, &m);
        }
        if (g_recorder.state != REC_STATE_RECORDING) {
            break;
        }
        if (pending_len && (int32_t)(now_ms - last_flush_ms) >= (int32_t)PHOTON_REC_FLUSH_MS) {
            if (!flush(now_ms)) {
                break;
            }
        }
        {
            // Signed: a message stamped by core 0 an instant after core 1
            // sampled now_ms must not read as 49 days of silence.
            int32_t quiet = (int32_t)(now_ms - last_event_ms);
            if (quiet >= (int32_t)PHOTON_REC_SILENCE_MS &&
                (held_count == 0 || quiet >= (int32_t)PHOTON_REC_HELD_CAP_MS)) {
                close_file(now_ms);
            }
        }
        break;

    case REC_STATE_STOPPED:
    default:
        drain();
        break;
    }
}

void recorder_init(void) {
    g_recorder.enabled = 0;
    if (file_open) {
        f_close(&fil);
        file_open = false;
    }
    if (mounted) {
        f_unmount("");
        mounted = false;
    }
    ring.head = 0;
    ring.tail = 0;
    mount_tried = false;
    next_dir = 0;
    data_end = 0;
    reset_episode();
    memset(&g_recorder, 0, sizeof g_recorder);
    g_recorder.state = REC_STATE_NO_CARD;
    __dmb();
    g_recorder.enabled = 1;
}

const char *recorder_state_name(uint8_t state) {
    switch (state) {
    case REC_STATE_NO_CARD:   return "no-card";
    case REC_STATE_IDLE:      return "idle";
    case REC_STATE_RECORDING: return "recording";
    case REC_STATE_STOPPED:   return "stopped";
    default:                  return "?";
    }
}

#ifndef PHOTON_HOST_TEST
#include "pico/time.h"

void recorder_core1_entry(void) {
    for (;;) {
        recorder_poll((uint32_t)(time_us_64() / 1000u));
        sleep_ms(1);
    }
}
#endif
