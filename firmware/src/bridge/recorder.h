// Black-box MIDI recorder on the bridge's microSD card. Everything midi_out
// emits also lands on the card as Standard MIDI Files, host or no host:
//
//   NNNN/           one directory per power-on, created on the FIRST NOTE
//                   (never at boot, so idle power cycles leave no trace)
//   NNNN/MMMM.MID   one file per playing episode: opened on the first note,
//                   closed after PHOTON_REC_SILENCE_MS with no key held
//
// Numbers run 0001-9999 and the recorder simply stops at 9999 (no wrap, no
// overwrite). The board has no clock, so instead of a date each file opens
// with a text meta event giving the time since power-on; within a file the
// delta times are exact milliseconds (SMPTE 25x40 division).
//
// Power can vanish at any moment, so every flush (PHOTON_REC_FLUSH_MS) leaves
// a complete, valid file behind: end-of-track marker and track length are
// rewritten on each flush. A cut cable costs at most the last flush interval.
//
// Threading: core 0 (midi_out) pushes into an SPSC ring; core 1 owns the
// card, FatFs, and every blocking SPI transfer, so SD latency never touches
// the bus poll cycle. Host tests drive recorder_poll() with a fake clock and
// a RAM disk (PHOTON_HOST_TEST).
#ifndef PHOTON_RECORDER_H
#define PHOTON_RECORDER_H

#include <stdbool.h>
#include <stdint.h>

#define PHOTON_REC_SILENCE_MS       30000u   // close the file after this quiet
#define PHOTON_REC_HELD_CAP_MS      300000u  // ... even with a key still down
#define PHOTON_REC_FLUSH_MS         500u     // worst case lost at power cut
#define PHOTON_REC_MOUNT_RETRY_MS   2000u    // no-card probe cadence
#define PHOTON_REC_MAX_NUMBER       9999u

typedef enum {
    REC_STATE_NO_CARD = 0,  // nothing mounted; retrying
    REC_STATE_IDLE,         // card mounted, no file open
    REC_STATE_RECORDING,    // file open
    REC_STATE_STOPPED,      // 9999 reached or card full; nothing more written
} recorder_state_t;

// Written by core 1, read by core 0 (console). Individual words only; never
// read as a consistent snapshot.
typedef struct {
    volatile uint8_t enabled;      // producer gate: set by recorder_init()
    volatile uint8_t state;        // recorder_state_t
    volatile uint8_t fs_type;      // FatFs FS_FAT16/FS_FAT32/FS_EXFAT
    volatile uint16_t next_dir;    // decided at mount: highest on card + 1
    volatile uint16_t dir_num;     // 0 until the first note of this power-on
    volatile uint16_t file_num;    // current/last file in dir_num
    volatile uint32_t files_closed;
    volatile uint32_t events_written;
    volatile uint32_t bytes_written;
    volatile uint32_t ring_drops;  // core 0 producer overflow
    volatile uint32_t errors;      // card errors that forced a remount
    volatile uint32_t card_mb;
    volatile uint32_t free_mb;     // at mount time
    const char *volatile last_error;  // string literal or NULL
} recorder_status_t;

extern recorder_status_t g_recorder;

// Core 0, bridge role only: reset state and open the producer gate.
void recorder_init(void);

// Core 0: called for every emitted MIDI channel message. No-op until
// recorder_init(); drops (counted) if core 1 falls behind by 512 messages.
bool recorder_push(uint32_t t_ms, uint8_t status, uint8_t d1, uint8_t d2);

// Core 1 (or a host test): one step of the state machine at time now_ms.
void recorder_poll(uint32_t now_ms);

const char *recorder_state_name(uint8_t state);

#ifndef PHOTON_HOST_TEST
// Core 1 entry: loops recorder_poll forever.
void recorder_core1_entry(void);
#endif

#endif
