/**
 * @file data_logger.c
 * @brief Capability-tagged session logger.
 *
 * Persists raw streams to flash with per-session capability flags so the
 * cloud-side training pipeline can filter sessions by what was actually
 * captured. See data_logger.h for the rationale.
 *
 * TODO:
 *   1. Choose backing store. Options:
 *      - SPIFFS / LittleFS partition (simple, well-trodden).
 *      - Raw flash partition with custom append-only log (faster, less FS overhead).
 *      Recommend LittleFS for V1 — robust and ESP-IDF supported.
 *   2. Implement session header write at session_open.
 *   3. Implement frame appender with stream_id tag.
 *   4. Implement the session upload surface defined by the BLE contract:
 *      ListSessionsRequest -> SessionList, ReadSessionRequest -> SessionChunk
 *      series, DeleteSessionRequest (../onecollar-platform/contracts/
 *      ble_protocol.proto §Session capture). drivers/ble_service.c already
 *      dispatches these and returns STATUS_NOT_READY until this lands.
 *   5. Per-session size cap and rotation policy.
 */

#include "data_logger.h"
#include "board.h"
#include "hal/imu_iface.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "data_logger";

#define DATALOG_MAGIC               0x4F43534Eu  // "ONCS" little-endian
#define DATALOG_HEADER_VERSION      1

static uint32_t s_active_capabilities = 0;
static bool     s_session_open = false;

/**
 * Compute capability flags from board.h compile-time defines plus any
 * runtime-detected sensor health. Called at session_open.
 */
static uint32_t compute_capabilities(void)
{
    uint32_t caps = 0;
#if BOARD_HAS_IMU_LOWG
    caps |= DATALOG_CAP_IMU_LOWG;
#endif
#if BOARD_HAS_IMU_HIGHG
    caps |= DATALOG_CAP_IMU_HIGHG;
#endif
#if BOARD_HAS_IMU_SFLP
    caps |= DATALOG_CAP_IMU_SFLP;
#endif
#if BOARD_HAS_IMU_MLC
    caps |= DATALOG_CAP_IMU_MLC;
#endif
#if BOARD_HAS_AUDIO
    caps |= DATALOG_CAP_AUDIO;
#endif
#if BOARD_HAS_VAD
    caps |= DATALOG_CAP_VAD;
#endif
#if BOARD_HAS_GPS
    caps |= DATALOG_CAP_GPS;
#endif
#if BOARD_HAS_BAROMETER
    caps |= DATALOG_CAP_BAROMETER;
#endif
    // TODO: clear bits for sensors that failed init at runtime
    return caps;
}

esp_err_t data_logger_init(void)
{
    ESP_LOGI(TAG, "data_logger_init");
    // TODO: mount LittleFS partition, prepare session directory
    return ESP_OK;
}

esp_err_t data_logger_session_open(datalog_session_header_t *out_header)
{
    if (!out_header) return ESP_ERR_INVALID_ARG;
    if (s_session_open) return ESP_ERR_INVALID_STATE;

    memset(out_header, 0, sizeof(*out_header));
    out_header->magic = DATALOG_MAGIC;
    out_header->header_version = DATALOG_HEADER_VERSION;
    out_header->hw_revision = BOARD_HW_REV;
    out_header->session_id = (uint64_t)esp_timer_get_time() * 1000ULL;
    /* The system clock is UTC iff the app has issued SetWallClockRequest
     * (contract opcode, handled in drivers/ble_service.c via settimeofday).
     * An unset RTC reads as 1970; anything before 2020 means "no sync yet"
     * and the header keeps the schema's documented 0. */
    {
        struct timeval tv;
        if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 1577836800 /* 2020-01-01 */) {
            out_header->wall_clock_start_ms =
                (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
        } else {
            out_header->wall_clock_start_ms = 0;
        }
    }
    out_header->capability_flags = compute_capabilities();

    {
        imu_info_t info;
        if (imu_get_info(&info) == ESP_OK) {
            out_header->imu_odr_hz = info.odr_hz;
            if (info.part_name) {
                strncpy(out_header->part_imu, info.part_name,
                        sizeof(out_header->part_imu) - 1);
            }
        }
    }
    out_header->audio_sample_rate_hz =
#if BOARD_HAS_AUDIO
        BOARD_I2S_SAMPLE_RATE;
#else
        0;
#endif
    strncpy(out_header->firmware_version, "0.0.0-scaffold", sizeof(out_header->firmware_version) - 1);

    s_active_capabilities = out_header->capability_flags;
    s_session_open = true;

    ESP_LOGI(TAG, "session opened: id=%llu caps=0x%08x", out_header->session_id, out_header->capability_flags);
    // TODO: persist header to flash
    return ESP_OK;
}

esp_err_t data_logger_session_close(void)
{
    if (!s_session_open) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "session closed");
    s_session_open = false;
    s_active_capabilities = 0;
    // TODO: flush + finalize on-disk session
    return ESP_OK;
}

esp_err_t data_logger_append(datalog_stream_id_t stream_id,
                             const void *payload, size_t payload_size)
{
    (void)stream_id; (void)payload; (void)payload_size;
    if (!s_session_open) return ESP_ERR_INVALID_STATE;
    // TODO: write [stream_id:1][length:4 LE][payload] to active session file
    return ESP_OK;
}

uint32_t data_logger_current_capabilities(void)
{
    return s_session_open ? s_active_capabilities : 0;
}
