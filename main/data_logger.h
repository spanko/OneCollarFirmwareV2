/**
 * @file data_logger.h
 * @brief Session-oriented data logger with per-session capability tagging.
 *
 * Sessions are the unit of training data. Each session captures raw streams
 * (IMU, audio when present, Tier 0 events, MLC features) at full fidelity.
 * Windowing happens at training time, not capture time — this preserves
 * Option B optionality (multi-task learning with pose supervision) and any
 * future training approach that needs different windowing than the launch
 * supervised classifier.
 *
 * Each session is written with a header that records which capabilities were
 * available at capture time. The cloud-side training pipeline reads these
 * flags to filter sessions for training jobs:
 *   - Audio-aware models train only on sessions with DATALOG_CAP_AUDIO.
 *   - Impact-detection models require DATALOG_CAP_IMU_HIGHG.
 *   - SFLP-derived feature models prefer DATALOG_CAP_IMU_SFLP but can fall
 *     back to host-computed gravity from sessions without it.
 *
 * This is the core mechanism that lets Rev 6 sessions and Rev 7 sessions
 * coexist in the same data lake without the training pipeline needing
 * special-case rev knowledge.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Per-session capability bitfield. Set by the data logger at session-open
 * time based on the active board's BOARD_HAS_* flags and at-runtime sensor
 * health checks.
 */
typedef enum {
    DATALOG_CAP_IMU_LOWG       = 1u << 0,   ///< baseline 6-DOF accel + gyro
    DATALOG_CAP_IMU_HIGHG      = 1u << 1,   ///< Rev 7+ dedicated high-g channel
    DATALOG_CAP_IMU_SFLP       = 1u << 2,   ///< Rev 7+ chip-native gravity/quat
    DATALOG_CAP_IMU_MLC        = 1u << 3,   ///< both revs (8 trees, 256 nodes)
    DATALOG_CAP_AUDIO          = 1u << 4,   ///< Rev 7+ I2S MEMS mic
    DATALOG_CAP_VAD            = 1u << 5,   ///< Rev 7 evaluation: Infineon PDM+VAD
    DATALOG_CAP_GPS            = 1u << 6,
    DATALOG_CAP_BAROMETER      = 1u << 7,   ///< both DNP currently
    // 8..31 reserved
} datalog_capability_flags_t;

/**
 * Session header written once per session at open. Fixed size, versioned.
 * All multi-byte integers are little-endian on the wire.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;                  ///< 0x4F43534E ("ONCS")
    uint16_t header_version;         ///< schema version; bump on incompatible changes
    uint16_t hw_revision;            ///< 6 or 7
    uint64_t session_id;             ///< nanosecond board-epoch start time
    uint64_t wall_clock_start_ms;    ///< if NTP/BLE-time-sync available; 0 otherwise
    uint32_t capability_flags;       ///< datalog_capability_flags_t bitfield
    uint16_t imu_odr_hz;
    uint16_t audio_sample_rate_hz;   ///< 0 if no audio
    char     part_imu[16];           ///< e.g. "LSM6DSO32X" / "LSM6DSV320X"
    char     firmware_version[16];   ///< git describe or semver
    uint8_t  reserved[64];           ///< pad to fixed size; future-proof
} datalog_session_header_t;

/**
 * Stream identifiers. Each stream is appended to the session as
 * length-prefixed frames tagged with stream_id.
 */
typedef enum {
    DATALOG_STREAM_IMU_LOWG        = 0x01,
    DATALOG_STREAM_IMU_HIGHG       = 0x02,
    DATALOG_STREAM_IMU_SFLP        = 0x03,
    DATALOG_STREAM_IMU_MLC_EVENT   = 0x04,
    DATALOG_STREAM_AUDIO_PCM       = 0x05,
    DATALOG_STREAM_VAD_EVENT       = 0x06,
    DATALOG_STREAM_GPS_FIX         = 0x07,
    DATALOG_STREAM_BATTERY         = 0x08,
    DATALOG_STREAM_USER_LABEL      = 0x10,  ///< from app via BLE during capture
    DATALOG_STREAM_ANNOTATION      = 0x11,  ///< driver-emitted notes (errors etc.)
} datalog_stream_id_t;

esp_err_t data_logger_init(void);
esp_err_t data_logger_session_open(datalog_session_header_t *out_header);
esp_err_t data_logger_session_close(void);

/**
 * Append a frame to the active session. Frame format on the wire:
 *   [stream_id:1][length:4 LE][payload:length bytes]
 * Caller is responsible for payload format per stream.
 */
esp_err_t data_logger_append(datalog_stream_id_t stream_id,
                             const void *payload,
                             size_t payload_size);

/** Returns the current session capability bitfield, or 0 if no session is open. */
uint32_t data_logger_current_capabilities(void);

/** True once the sessions LittleFS partition is mounted (capture available). */
bool data_logger_fs_ready(void);

// ---------------------------------------------------------------------------
// Session read-back (Stage C). The on-flash IMU frame payload is a persisted
// ImuBatch: [base_timestamp_us:8 LE][rate_hz:4 LE][sample_count:2 LE][N x 12B].
// ---------------------------------------------------------------------------

/** Summary of one stored session (proto-agnostic; ble_service maps to SessionSummary). */
typedef struct {
    uint64_t session_id;
    uint64_t start_time_us;     ///< first IMU frame's base timestamp (collar clock)
    uint64_t duration_us;       ///< last frame base - first frame base
    uint32_t sample_count;      ///< total IMU samples across the session
    uint32_t rate_hz;           ///< IMU rate (from the first IMU frame)
    uint32_t capability_flags;  ///< DATALOG_CAP_* at capture time (header)
    uint64_t size_bytes;        ///< on-flash file size
} datalog_session_info_t;

/** List stored sessions into out[] (up to max). Returns the count found. */
int data_logger_list(datalog_session_info_t *out, int max);

/**
 * Per-IMU-chunk callback for data_logger_read_imu. `samples_le` is
 * `sample_count` x (ax,ay,az,gx,gy,gz) LE int16 (12 B each). Chunks are split so
 * `sample_count` <= 40 (the SessionChunk wire cap). Return false to abort the read.
 */
typedef bool (*datalog_imu_frame_cb)(uint64_t base_us, uint32_t rate_hz,
                                     uint32_t sample_count,
                                     const uint8_t *samples_le, void *ctx);

/**
 * Read a stored session's IMU stream in [start_us, end_us] (0 = open end),
 * invoking `cb` per <=40-sample chunk. ESP_ERR_NOT_FOUND if no such session,
 * ESP_ERR_INVALID_STATE if it is the currently-capturing session.
 */
esp_err_t data_logger_read_imu(uint64_t session_id, uint64_t start_us,
                               uint64_t end_us, datalog_imu_frame_cb cb, void *ctx);

/** Delete a stored session. INVALID_STATE if it is the active capture. */
esp_err_t data_logger_delete(uint64_t session_id);

#ifdef __cplusplus
}
#endif
