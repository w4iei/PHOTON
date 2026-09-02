// Single source of truth for pins, peripherals, timing and detection
// constants. Values migrated from software/embedded_software/src/app/
// rs485_system_config.py and the sensor board 002 Rev D schematic.
#ifndef PHOTON_BOARD_CONFIG_H
#define PHOTON_BOARD_CONFIG_H

#include "hardware/spi.h"
#include "hardware/uart.h"

// ---------------------------------------------------------------------------
// Sensor array topology (sensor board 002)
// ---------------------------------------------------------------------------
#define PHOTON_BANK_COUNT        8   // TLA2518 chips
#define PHOTON_SLOTS_PER_BANK    4   // sensors per bank
#define PHOTON_MAX_SENSORS       (PHOTON_BANK_COUNT * PHOTON_SLOTS_PER_BANK)  // 32
#define PHOTON_ACTIVE_SENSORS    31  // last slot unpopulated by default

// Per-bank slot map (identical for all banks):
//   slot 0 -> AIN7, emitter GPO bit 6      slot 2 -> AIN3, emitter GPO bit 2
//   slot 1 -> AIN5, emitter GPO bit 4      slot 3 -> AIN1, emitter GPO bit 0
#define PHOTON_SLOT_ADC_CHANNELS { 7, 5, 3, 1 }
#define PHOTON_SLOT_EMITTER_BITS { 6, 4, 2, 0 }
#define PHOTON_EMITTER_MASK_ALL  0x55u

// SPI buses. NOTE the historical software names are swapped relative to the
// hardware peripherals ("spi0" in Python config = hardware SPI1 and vice
// versa). Native code uses hardware names only:
//   bus A = hardware SPI1: SCLK GPIO10, MOSI GPIO11, MISO GPIO8, banks 0-3
//   bus B = hardware SPI0: SCLK GPIO2,  MOSI GPIO3,  MISO GPIO0, banks 4-7
#define PHOTON_SPI_A             spi1
#define PHOTON_SPI_A_SCLK        10
#define PHOTON_SPI_A_MOSI        11
#define PHOTON_SPI_A_MISO        8
#define PHOTON_SPI_B             spi0
#define PHOTON_SPI_B_SCLK        2
#define PHOTON_SPI_B_MOSI        3
#define PHOTON_SPI_B_MISO        0

// Bank -> CS GPIO, banks 0-7. Banks 0-3 on bus A, 4-7 on bus B.
#define PHOTON_BANK_CS_PINS      { 21, 20, 19, 15, 1, 7, 5, 6 }
#define PHOTON_BANK_ON_BUS_B(b)  ((b) >= 4)

// Validated bus speed (2026-09-02). Faster clocks lost register writes to
// the ADC nearest the MCU (near-end reflection on the sensor board); see
// firmware/README.md "Validated settings". Mode-2 sweep 969 us.
#define PHOTON_SPI_BAUD_HZ       (10 * 1000 * 1000)
// Emitter settle before sampling. Bench-measured 2026-08-13: the reading
// depends strongly on this value with keys installed (45 us reads 81-91% of
// 60 us), so the VCNT2025X01 response is still developing well past 30 us.
// An earlier 30 us figure came from a board measured while DISASSEMBLED with
// keys missing - the sensors were staring at nothing, which is not a
// representative optical condition. 50 us keeps close to the long-proven
// 60 us while shortening the sequential sweep (~2533 -> ~2213 us).
// Changing this shifts every reading and REQUIRES recalibration.
#define PHOTON_SETTLE_US         50
#define PHOTON_OSR_MODE          3       // TLA2518 8x oversample, 16-bit results
#define PHOTON_TLA_RESET_WAIT_US 10000   // datasheet requires 5 ms; keep proven 10 ms
#define PHOTON_TLA_BUS_FLUSH_US  10

// ---------------------------------------------------------------------------
// Event engine (values proven in production CircuitPython build)
// ---------------------------------------------------------------------------
#define PHOTON_STRIKE_PCT            60
#define PHOTON_RELEASE_PCT           40
#define PHOTON_VELOCITY_WINDOW_PCT   20
#define PHOTON_STRIKE_WINDOW_PCT     30
#define PHOTON_MIN_EVENT_RANGE       170  // scaled by << osr_mode at runtime (1360 @ OSR 3)

// ---------------------------------------------------------------------------
// RS-485 transport (current hardware, Plan 1)
// ---------------------------------------------------------------------------
#define PHOTON_RS485_UART        uart1
#define PHOTON_RS485_TX          22   // F11 alternate UART1 mapping
#define PHOTON_RS485_RX          23
#define PHOTON_RS485_DE          24
#define PHOTON_RS485_TERM        25
#define PHOTON_RS485_BAUD        4000000  // exact divisor at clk_peri 150 MHz (2.34375)
#define PHOTON_DE_GUARD_US       8    // guard band before/after each frame; THVD1424
                                      // enable time is <100 ns, 8 us is still ample
                                      // (25 us proven first, tightened in bench tuning)

// Main controller board 001 uses different RS-485 pins; probed at runtime:
// if the sensor capability probe finds zero banks, the transport uses these.
#define PHOTON_HOST_UART         uart1
#define PHOTON_HOST_TX           4    // F2 UART1 mapping
#define PHOTON_HOST_RX           5
#define PHOTON_HOST_DE           1
#define PHOTON_HOST_TERM         18

#define PHOTON_MAX_NODE_ID       6
#define PHOTON_POLL_TIMEOUT_US   700  // > worst-case 158 B reply @ 4 Mbaud (395 us) + guards
#define PHOTON_POLL_RETRIES      2
#define PHOTON_PING_INTERVAL_MS  250  // re-discovery cadence for silent ids
// Bridge poll-cycle pacing: minimum period of one full rotation over all
// alive nodes. 0 = free-run. 1000 us halves the bridge's transmitter duty
// (and its LDO heat) while keeping worst-case event latency ~1 ms.
#define PHOTON_POLL_CYCLE_US     1000

// ---------------------------------------------------------------------------
// Frame format v2
// ---------------------------------------------------------------------------
#define PHOTON_FRAME_MAX_PAYLOAD 128
#define PHOTON_FRAME_HEADER_LEN  10   // A5 5A type flags src dst len seq(2) crc8
#define PHOTON_FRAME_CRC_LEN     4
#define PHOTON_FRAME_MAX_LEN     (PHOTON_FRAME_HEADER_LEN + PHOTON_FRAME_MAX_PAYLOAD + PHOTON_FRAME_CRC_LEN)

// ---------------------------------------------------------------------------
// Inter-core IPC
// ---------------------------------------------------------------------------
#define PHOTON_EVENT_RING_SLOTS  256  // 8x the worst 32-event chord burst
#define PHOTON_CMD_MAILBOX_SLOTS 16

// ---------------------------------------------------------------------------
// MIDI mapping (bridge role)
// ---------------------------------------------------------------------------
#define PHOTON_MIDI_LOW          29   // F1
#define PHOTON_MIDI_HIGH         89   // F6
#define PHOTON_MIDI_CHANNEL      1    // 0-based wire channel (user-facing "2")
#define PHOTON_SENSORS_PER_MANUAL 64  // BOARD_PAIR_SIZE(2) boards x 32 slots;
                                      // note range + MIDI channel restart per manual
#define PHOTON_MAX_MANUALS       ((PHOTON_MAX_NODE_ID * PHOTON_MAX_SENSORS + \
                                   PHOTON_SENSORS_PER_MANUAL - 1) / PHOTON_SENSORS_PER_MANUAL)
#define PHOTON_TRACE_DEFAULT_SENSOR 24  // legacy bench default (SENSOR_IDX)
#define PHOTON_CAPTURE_DEFAULT_S    3   // legacy CAPTURE_SECONDS

// Default paced sweep rate (config scan_rate_hz==0 selects this; 0xFFFF in
// config = unthrottled). Production is two-phase at 600 Hz: 1.67 ms dt
// quantization (55 velocity steps across the 8-100 ms window) for +0.2 W
// over 300 Hz, measured 2026-09-02 on the buck-regulated rev 1D system.
// Mode 2's 826 us sweep leaves ~50% idle at this rate.
#define PHOTON_DEFAULT_SCAN_RATE_HZ 600
// Velocity curve, fitted 2026-09-02 to 1,587 measured strikes (pp / mf-f /
// ff medians 24.9 / 9.9 / 3.3 ms — each dynamic ~2.5x the next, so the map
// is logarithmic in dt, skewed by VEL_CURVE to hold the top end up):
//   x = log(dt / MIN_MS) / log(MAX_MS / MIN_MS)   (0 at MIN_MS, 1 at MAX_MS)
//   v = OUT_MAX - (OUT_MAX - OUT_MIN) * x^VEL_CURVE
// Lands pp / mf-f / ff at 54 / 95 / 119. Runtime: 'velcurve <min_ms> <max_ms>
// <gamma>'. See firmware/README.md "Validated settings".
#define PHOTON_VEL_MIN_MS        2.5f   // dt <= 2.5 ms -> OUT_MAX
#define PHOTON_VEL_MAX_MS        25.0f  // dt >= 25 ms  -> OUT_MIN
#define PHOTON_VEL_CURVE         2.0f   // gamma on the log position
// MIDI velocity output range. A harpsichord plucks the same way regardless of
// key speed, so loudness is essentially independent of touch; emitting the
// full 1-127 makes a piano sample library treat soft strikes as "barely sound
// the note". Compressing the curve's output into a band that always speaks
// costs nothing: 300 Hz resolves only ~27 distinct dt steps, which is ~1.5
// velocity units across 75-115 — finer than anyone can hear.
#define PHOTON_VEL_OUT_MIN       50.0f  // pp; the instrument's lower bound
#define PHOTON_VEL_OUT_MAX       120.0f // ff
#define PHOTON_GLOBAL_SENSORS    (PHOTON_MAX_NODE_ID * PHOTON_MAX_SENSORS)
// Default disabled global sensor indices (base config: 31, 62, 63 for the
// historical 2-board setup) live in the config store defaults.

// ---------------------------------------------------------------------------
// USB identity
// ---------------------------------------------------------------------------
#define PHOTON_USB_VID           0x1B4F
#define PHOTON_USB_PID           0x0039  // native stack (CircuitPython builds use 0x0038)

// ---------------------------------------------------------------------------
// Config store: two 4 KB sectors at the top of the 16 MB flash
// ---------------------------------------------------------------------------
#define PHOTON_CONFIG_SECTOR_SIZE 4096u
#define PHOTON_CONFIG_FLASH_OFFS  (PICO_FLASH_SIZE_BYTES - 2u * PHOTON_CONFIG_SECTOR_SIZE)
// Where the config lived when the firmware was built for a 16 MB part. Read
// only, for one-way migration onto the flash-size-agnostic location above.
// Harmless on a 2 MB device: the address is inside the 16 MB XIP window, and
// the flash simply wraps, so the CRC check rejects whatever comes back.
#define PHOTON_CONFIG_LEGACY_OFFS (16u * 1024u * 1024u - 2u * PHOTON_CONFIG_SECTOR_SIZE)

// ---------------------------------------------------------------------------
// microSD (bridge boards 001 / 001D): SPI mode on hardware SPI1. The socket's
// DAT3/CD pin is the chip select, so there is no card-detect switch; presence
// is discovered by the init handshake (recorder retries every 2 s).
// ---------------------------------------------------------------------------
#define PHOTON_SD_SPI            spi1
#define PHOTON_SD_SCK            10   // CLK
#define PHOTON_SD_MOSI           11   // CMD
#define PHOTON_SD_MISO           12   // DAT0
#define PHOTON_SD_CS             13   // DAT3/CD
#define PHOTON_SD_BAUD_INIT_HZ   (250 * 1000)         // spec: <= 400 kHz until ready
#define PHOTON_SD_BAUD_HZ        (12500 * 1000)       // 150 MHz / 12; SPI-mode max is 25

#endif // PHOTON_BOARD_CONFIG_H
