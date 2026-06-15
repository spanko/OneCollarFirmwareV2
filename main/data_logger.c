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
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "data_logger";

#define DATALOG_MAGIC               0x4F43534Eu  // "ONCS" little-endian
#define DATALOG_HEADER_VERSION      1

// LittleFS on the dedicated `sessions` partition (partitions.csv). One file per
// session: [header][frame][frame]... where each frame is
// [stream_id:1][len:4 LE][payload] (data_logger.h).
#define SESSIONS_PARTITION_LABEL    "sessions"
#define SESSIONS_BASE_PATH          "/sessions"
#define SESSION_PATH_MAX            48

static uint32_t s_active_capabilities = 0;
static bool     s_session_open = false;
static bool     s_fs_mounted = false;
static FILE    *s_file = NULL;        // active session's open file (append)
static uint64_t s_active_session_id = 0;
// Serializes file ops: during a capture the IMU logger task and the host-task
// LabelTag handler both append to s_file; close must not race either.
static SemaphoreHandle_t s_mux = NULL;

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
    if (!s_mux) s_mux = xSemaphoreCreateMutex();
    if (!s_mux) return ESP_ERR_NO_MEM;
    esp_vfs_littlefs_conf_t conf = {
        .base_path = SESSIONS_BASE_PATH,
        .partition_label = SESSIONS_PARTITION_LABEL,
        .format_if_mount_failed = true,   // first boot / corrupt: format and continue
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        // Non-fatal: capture is unavailable but the collar still runs (telemetry,
        // streaming-to-phone). data_logger_session_open will refuse cleanly.
        ESP_LOGE(TAG, "LittleFS mount failed on '%s' (%s) — capture disabled",
                 SESSIONS_PARTITION_LABEL, esp_err_to_name(err));
        return err;
    }
    s_fs_mounted = true;
    size_t total = 0, used = 0;
    if (esp_littlefs_info(SESSIONS_PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "sessions FS mounted: %u/%u KB used", (unsigned)(used / 1024),
                 (unsigned)(total / 1024));
    }
    return ESP_OK;
}

esp_err_t data_logger_session_open(datalog_session_header_t *out_header)
{
    if (!out_header) return ESP_ERR_INVALID_ARG;
    if (s_session_open) return ESP_ERR_INVALID_STATE;
    if (!s_fs_mounted) return ESP_ERR_INVALID_STATE;  // no FS → no capture

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

    // Create the session file and write the header at offset 0. The file stays
    // open for frame appends until session_close.
    char path[SESSION_PATH_MAX];
    snprintf(path, sizeof(path), SESSIONS_BASE_PATH "/%" PRIu64 ".ocs",
             out_header->session_id);
    s_file = fopen(path, "wb");
    if (!s_file) {
        ESP_LOGE(TAG, "cannot create %s", path);
        return ESP_FAIL;
    }
    if (fwrite(out_header, 1, sizeof(*out_header), s_file) != sizeof(*out_header)) {
        ESP_LOGE(TAG, "header write failed");
        fclose(s_file);
        s_file = NULL;
        return ESP_FAIL;
    }
    fflush(s_file);

    s_active_capabilities = out_header->capability_flags;
    s_active_session_id = out_header->session_id;
    s_session_open = true;

    ESP_LOGI(TAG, "session opened: id=%" PRIu64 " caps=0x%08" PRIx32 " -> %s",
             out_header->session_id, out_header->capability_flags, path);
    return ESP_OK;
}

esp_err_t data_logger_session_close(void)
{
    if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY);
    if (!s_session_open) {
        if (s_mux) xSemaphoreGive(s_mux);
        return ESP_ERR_INVALID_STATE;
    }
    long size = -1;
    if (s_file) {
        fflush(s_file);
        size = ftell(s_file);
        fclose(s_file);
        s_file = NULL;
    }
    ESP_LOGI(TAG, "session %" PRIu64 " closed (%ld bytes)", s_active_session_id, size);
    s_session_open = false;
    s_active_capabilities = 0;
    s_active_session_id = 0;
    if (s_mux) xSemaphoreGive(s_mux);
    return ESP_OK;
}

esp_err_t data_logger_append(datalog_stream_id_t stream_id,
                             const void *payload, size_t payload_size)
{
    if (payload_size > 0 && !payload) return ESP_ERR_INVALID_ARG;
    if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY);
    if (!s_session_open || !s_file) {
        if (s_mux) xSemaphoreGive(s_mux);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t rc = ESP_OK;
    // Frame: [stream_id:1][len:4 LE][payload]
    uint8_t hdr[5];
    hdr[0] = (uint8_t)stream_id;
    uint32_t len = (uint32_t)payload_size;
    hdr[1] = (uint8_t)(len & 0xFF);
    hdr[2] = (uint8_t)((len >> 8) & 0xFF);
    hdr[3] = (uint8_t)((len >> 16) & 0xFF);
    hdr[4] = (uint8_t)((len >> 24) & 0xFF);
    if (fwrite(hdr, 1, sizeof(hdr), s_file) != sizeof(hdr)) {
        rc = ESP_FAIL;
    } else if (payload_size > 0 &&
               fwrite(payload, 1, payload_size, s_file) != payload_size) {
        rc = ESP_FAIL;
    }
    if (s_mux) xSemaphoreGive(s_mux);
    return rc;
}

uint32_t data_logger_current_capabilities(void)
{
    return s_session_open ? s_active_capabilities : 0;
}

bool data_logger_fs_ready(void)
{
    return s_fs_mounted;
}
