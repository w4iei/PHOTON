// Knee calibration on a sensor board, core 0. Follows the capture sessions
// core 1 runs while calibration learning is on (core1/calcap.c): analyses
// each finished swing for its knee (cal/knee.c), keeps a status per key for
// the live keyboard view, keeps each key's last full swing for 'cal dump',
// remembers the calibration that was in force for 'cal compare', and on
// save turns every key's knee into its strike threshold.
#ifndef PHOTON_CAL_SESSION_H
#define PHOTON_CAL_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"
#include "cal/knee.h"

typedef enum {
    CAL_KEY_UNSEEN = 0,  // no full press captured yet
    CAL_KEY_OK,          // knee found: green
    CAL_KEY_NO_KNEE,     // full press without a knee: press slower
    CAL_KEY_ADJACENT,    // a neighbour went down at the same time: redo alone
    CAL_KEY_COUPLED,     // bridge overlay: the other manual moved with it
    CAL_KEY_DISABLED,    // masked or unpopulated
} cal_key_status_t;

// Flags of a kept swing.
#define CAL_SWING_VALID     0x01
#define CAL_SWING_KNEE      0x02
#define CAL_SWING_TRUNCATED 0x04
#define CAL_SWING_ADJACENT  0x08

typedef struct {
    uint8_t status;       // cal_key_status_t
    uint8_t n_knees;      // good knees kept (<= PHOTON_CAL_MAX_KNEES)
    uint8_t knee_head;
    uint16_t knees[PHOTON_CAL_MAX_KNEES];  // raw readings at each good knee
    uint32_t swings;      // full presses analysed this session
    // The last full swing, kept for 'cal dump', with its own analysis.
    uint16_t kept_len;
    uint16_t kept_pre;
    uint8_t kept_flags;   // CAL_SWING_*
    uint8_t kept_pct;     // knee, % of that swing's own range
    uint16_t kept_knee_idx;
    uint16_t kept_knee_value;
    uint16_t kept_rest;
    uint16_t kept_top;
    uint16_t kept_period_us;
    // Calibration in force when the session began ('cal compare').
    uint16_t old_min;
    uint16_t old_max;
    uint8_t old_strike;   // 0 = global PHOTON_STRIKE_PCT
} cal_key_t;

// Main loop, sensor role.
void cal_session_task(void);

bool cal_session_active(void);  // calibration learning is on
bool cal_session_have(void);    // a session ran since boot (compare/dump data)
uint8_t cal_session_status(uint8_t idx);
const cal_key_t *cal_session_key(uint8_t idx);
const uint16_t *cal_session_swing(uint8_t idx);  // kept samples

// Strike % the key's knees give against calibrated range mn..mx, margin
// included; 0 = no knee (the key keeps the global threshold).
uint8_t cal_session_strike(uint8_t idx, uint16_t mn, uint16_t mx);

// The knee rules in force ('cal rules', compiled defaults where unset) and
// the threshold margin above the knee.
knee_rules_t cal_session_rules(uint8_t *margin_pct);

// 'strike global': every key at PHOTON_STRIKE_PCT, and calibration turns a
// key green on any full press (no knee asked for). The knees are still
// found and saved, so switching back to 'strike knee' needs no new
// calibration.
bool cal_session_global(void);

// Hand core 1 the strike table the mode calls for: the saved per-key
// table, or every key global. At commit and after 'strike knee|global'.
void cal_session_apply_mode(void);

// 'cal rules' payload / arguments within range: window %, ratio x10,
// jump %, margin % (0 = compiled default for any of them).
bool cal_rules_valid(const uint8_t *r);

// Run the knee search again on every key's kept swing with the rules in
// force now ('cal rules' during calibration); each key then keeps only
// that swing's result. No-op outside a calibration.
void cal_session_reanalyse(void);

// Save path, after the freeze is pushed and before config_store_save():
// writes each key's strike % into g_config and stages it for core 1.
// No-op (false) when no calibration session is running, so a 'cal save'
// reaching a board that was not recalibrated leaves its thresholds alone.
bool cal_session_commit(void);

// ---------------------------------------------------------------------------
// Wire payloads. Built here so the local console and the bridge (over the
// bus) read exactly the same thing.
// ---------------------------------------------------------------------------

// First byte of the status and info payloads.
#define CAL_FLAG_LEARNING 0x01
#define CAL_FLAG_GLOBAL   0x02

// CAL_STATUS_RESP: flags u8 | n u8 | status[n] | strike[n]
// strike = the % a save would store now (0 = global).
uint8_t cal_session_build_status(uint8_t *out);

// CAL_INFO_RESP: start u8 | count u8 | flags u8 | count x cal_info_rec_t
typedef struct __attribute__((packed)) {
    uint8_t status;
    uint8_t new_strike;   // preview while calibrating, stored value after
    uint8_t old_strike;   // 0 = global
    uint8_t n_knees;
    uint16_t old_min;
    uint16_t old_max;
    uint16_t new_min;
    uint16_t new_max;
} cal_info_rec_t;
#define CAL_INFO_PER_FRAME 9
_Static_assert(sizeof(cal_info_rec_t) == 12, "cal info record is 12 bytes on the wire");
_Static_assert(3 + CAL_INFO_PER_FRAME * sizeof(cal_info_rec_t) <= PHOTON_FRAME_MAX_PAYLOAD,
               "cal info page must fit one frame");
_Static_assert(2 + 2 * PHOTON_ACTIVE_SENSORS <= PHOTON_FRAME_MAX_PAYLOAD,
               "cal status must fit one frame");
uint8_t cal_session_build_info(uint8_t start, uint8_t count, uint8_t *out);

// CAL_SWING_RESP: cal_swing_hdr_t | n x u16 samples from `offset`
typedef struct __attribute__((packed)) {
    uint8_t key;
    uint8_t flags;        // CAL_SWING_*
    uint16_t offset;
    uint16_t len;
    uint16_t pre;
    uint16_t period_us;
    uint16_t knee_idx;
    uint16_t knee_value;
    uint16_t rest;
    uint16_t top;
    uint8_t n;
    uint8_t status;       // the key's cal_key_status_t
} cal_swing_hdr_t;
_Static_assert(sizeof(cal_swing_hdr_t) == 20, "cal swing header is 20 bytes on the wire");
#define CAL_SWING_PER_FRAME ((PHOTON_FRAME_MAX_PAYLOAD - sizeof(cal_swing_hdr_t)) / 2)
uint8_t cal_session_build_swing(uint8_t key, uint16_t offset, uint8_t *out);

#endif
