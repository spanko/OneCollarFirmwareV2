/**
 * @file imu_lsm6dsv320x.c
 * @brief Rev 7 IMU backend — STMicro LSM6DSV320X.
 *
 * Implements imu_iface.h, sflp_iface.h, and highg_iface.h for Rev 7.
 *
 * Architecture: MLC, SFLP, FSM, and high-g channel are PARALLEL on-chip blocks
 * — they consume the same filtered upstream accel/gyro chain but do not feed
 * each other. ESP32-S3 reads results from each via FIFO tags or dedicated
 * registers and combines them in Tier 1.
 *
 * MLC: 8 trees, 256 nodes (same capacity as DSO32X). UCF blobs authored in ST
 * MEMS Studio (which supersedes Unico-GUI; same UCF on-the-wire format).
 *
 * Recommended driver foundation: ST's standardized C driver
 * (https://github.com/STMicroelectronics/lsm6dsv320x-pid). MEMS Studio exports
 * are also drop-in compatible with this driver layer.
 *
 * TODO (in rough order):
 *   1. Vendor or submodule lsm6dsv320x-pid into components/.
 *   2. Implement imu_init: WHO_AM_I check, low-g ODR + FS, FIFO setup,
 *      INT1 routing for MLC + FSM events.
 *   3. Implement sflp_init: enable SFLP block, configure quaternion + gravity
 *      output rates, route to FIFO with SFLP tag.
 *   4. Implement highg_init: enable second accel channel at desired FS
 *      (32 / 64 / 128 / 256 / 320 g), configure peak detector + FIFO tag.
 *   5. FIFO drain: parse multi-tag stream (low-g, gyro, SFLP, high-g, MLC).
 *   6. Empirically validate ~21 µA LPM2 current at 60 Hz (per datasheet).
 */

#include "imu_iface.h"
#include "sflp_iface.h"
#include "highg_iface.h"
#include "i2c_bus.h"
#include "board.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "imu_dsv320x";

#define LSM6DSV320X_WHO_AM_I_VALUE  0x73    // confirm against datasheet
#define LSM6DSV320X_REG_WHO_AM_I    0x0F

// ===========================================================================
// IMU iface (low-g + gyro + MLC)
// ===========================================================================

esp_err_t imu_init(void)
{
    ESP_LOGI(TAG, "imu_init: LSM6DSV320X @ I2C 0x%02X", BOARD_IMU_I2C_ADDR);
    // TODO: WHO_AM_I check, ODR/FS config, FIFO, INT routing
    return ESP_OK;
}

esp_err_t imu_deinit(void)              { return ESP_OK; }

esp_err_t imu_get_info(imu_info_t *out_info)
{
    if (!out_info) return ESP_ERR_INVALID_ARG;
    out_info->part_name = "LSM6DSV320X";
    out_info->who_am_i = LSM6DSV320X_WHO_AM_I_VALUE;
    out_info->mlc_tree_count = 8;
    out_info->mlc_node_count = 256;
    out_info->odr_hz = 60;  // LPM2 default per datasheet
    return ESP_OK;
}

esp_err_t imu_read_sample(imu_sample_t *out_sample)
{
    if (!out_sample) return ESP_ERR_INVALID_ARG;
    // TODO: read low-g UI channel (note: ±16 g max on DSV320X low-g)
    memset(out_sample, 0, sizeof(*out_sample));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t imu_read_sample_raw(imu_raw_sample_t *out_sample)
{
    if (!out_sample) return ESP_ERR_INVALID_ARG;
    // TODO: DSV320X low-g UI burst (gyro XYZ + accel XYZ) -> raw int16, accel-first.
    memset(out_sample, 0, sizeof(*out_sample));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t imu_drain_fifo(imu_sample_t *out_buffer, size_t buffer_capacity, size_t *out_count)
{
    (void)out_buffer; (void)buffer_capacity;
    if (out_count) *out_count = 0;
    // TODO: FIFO drain with multi-tag dispatch (low-g, high-g, SFLP, MLC)
    return ESP_OK;
}

esp_err_t imu_set_odr_hz(uint16_t odr_hz)
{
    ESP_LOGI(TAG, "imu_set_odr_hz: %u", odr_hz);
    return ESP_OK;
}

esp_err_t imu_mlc_load_program(const uint8_t *ucf_blob, size_t blob_size)
{
    ESP_LOGI(TAG, "imu_mlc_load_program (MEMS Studio UCF): %u bytes", (unsigned)blob_size);
    if (!ucf_blob || blob_size == 0) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

esp_err_t imu_mlc_register_callback(imu_mlc_callback_t callback, void *user_data)
{
    (void)callback; (void)user_data;
    return ESP_OK;
}

esp_err_t imu_mlc_set_enabled(bool enabled)
{
    ESP_LOGI(TAG, "imu_mlc_set_enabled: %d", enabled);
    return ESP_OK;
}

esp_err_t imu_set_wake_enabled(bool enabled) { (void)enabled; return ESP_OK; }
esp_err_t imu_enter_low_power(void)          { return ESP_OK; }
esp_err_t imu_exit_low_power(void)           { return ESP_OK; }

// ===========================================================================
// SFLP iface — chip-native gravity + quaternion
// ===========================================================================

esp_err_t sflp_init(void)
{
    ESP_LOGI(TAG, "sflp_init");
    // TODO: enable SFLP block, configure output rates, route to FIFO
    return ESP_OK;
}

esp_err_t sflp_deinit(void)                       { return ESP_OK; }
esp_err_t sflp_set_enabled(bool enabled)          { (void)enabled; return ESP_OK; }

esp_err_t sflp_read(sflp_sample_t *out_sample)
{
    if (!out_sample) return ESP_ERR_INVALID_ARG;
    // TODO: read SFLP_*_REG, convert half-precision floats to single-precision
    memset(out_sample, 0, sizeof(*out_sample));
    return ESP_ERR_NOT_FOUND;
}

// ===========================================================================
// High-g iface — dedicated ±320 g channel
// ===========================================================================

esp_err_t highg_init(void)
{
    ESP_LOGI(TAG, "highg_init");
    // TODO: enable high-g channel, set FS, configure peak detector
    return ESP_OK;
}

esp_err_t highg_deinit(void)                      { return ESP_OK; }
esp_err_t highg_set_enabled(bool enabled)         { (void)enabled; return ESP_OK; }

esp_err_t highg_read(highg_sample_t *out_sample)
{
    if (!out_sample) return ESP_ERR_INVALID_ARG;
    memset(out_sample, 0, sizeof(*out_sample));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t highg_set_peak_config(const highg_peak_config_t *config)
{
    (void)config;
    return ESP_OK;
}

esp_err_t highg_register_peak_callback(highg_peak_callback_t cb, void *user_data)
{
    (void)cb; (void)user_data;
    return ESP_OK;
}
