/**
 * @file imu_lsm6dso32x.c
 * @brief Rev 6 IMU backend — STMicro LSM6DSO32X.
 *
 * Implements imu_iface.h for Rev 6. SFLP and high-g ifaces are NOT implemented
 * here; the linker resolves their symbols from sflp_iface_stub.c when
 * BOARD_HAS_SFLP / BOARD_HAS_HIGHG are 0.
 *
 * MLC: 8 trees, 256 nodes. UCF blobs authored in ST Unico-GUI.
 *
 * Recommended driver foundation: ST's standardized C drivers
 * (https://github.com/STMicroelectronics/lsm6dso32x-pid). Do not hand-roll
 * register-level code — ST publishes a BSD-licensed C driver that handles the
 * low-level I/O cleanly and is reused by their own examples, MEMS Studio
 * exports, and the AlgoBuilder pipeline.
 *
 * TODO (in rough order):
 *   1. Vendor or submodule lsm6dso32x-pid into components/.
 *   2. Implement imu_init: WHO_AM_I check (0x6C), ODR config, FIFO setup,
 *      INT1 routing for MLC events.
 *   3. Wire INT1 GPIO ISR → FreeRTOS notification → MLC callback dispatch.
 *   4. Implement imu_drain_fifo with FIFO tag parsing.
 *   5. Implement imu_mlc_load_program by replaying the UCF register writes.
 *   6. Empirically validate ~15 µA Tier 0 current (per arch doc §11).
 */

#include "imu_iface.h"
#include "i2c_bus.h"
#include "board.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "imu_dso32x";

#define LSM6DSO32X_WHO_AM_I_VALUE   0x6C
#define LSM6DSO32X_REG_WHO_AM_I     0x0F

esp_err_t imu_init(void)
{
    ESP_LOGI(TAG, "imu_init: LSM6DSO32X @ I2C 0x%02X", BOARD_IMU_I2C_ADDR);
    // TODO: WHO_AM_I check
    // TODO: configure ODR, FIFO, INT routing
    // TODO: configure MLC INT1 GPIO + ISR
    return ESP_OK;
}

esp_err_t imu_deinit(void)
{
    return ESP_OK;
}

esp_err_t imu_get_info(imu_info_t *out_info)
{
    if (!out_info) return ESP_ERR_INVALID_ARG;
    out_info->part_name = "LSM6DSO32X";
    out_info->who_am_i = LSM6DSO32X_WHO_AM_I_VALUE;
    out_info->mlc_tree_count = 8;
    out_info->mlc_node_count = 256;
    out_info->odr_hz = 104;
    return ESP_OK;
}

esp_err_t imu_read_sample(imu_sample_t *out_sample)
{
    if (!out_sample) return ESP_ERR_INVALID_ARG;
    // TODO: read OUTX_L_A through OUTZ_H_G, convert LSB to mg / mdps.
    // Default ranges: ±32 g, ±2000 dps.
    memset(out_sample, 0, sizeof(*out_sample));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t imu_drain_fifo(imu_sample_t *out_buffer, size_t buffer_capacity, size_t *out_count)
{
    (void)out_buffer; (void)buffer_capacity;
    if (out_count) *out_count = 0;
    // TODO: FIFO_STATUS read, drain at FIFO tag granularity
    return ESP_OK;
}

esp_err_t imu_set_odr_hz(uint16_t odr_hz)
{
    ESP_LOGI(TAG, "imu_set_odr_hz: %u", odr_hz);
    // TODO: write CTRL1_XL / CTRL2_G ODR fields
    return ESP_OK;
}

esp_err_t imu_mlc_load_program(const uint8_t *ucf_blob, size_t blob_size)
{
    ESP_LOGI(TAG, "imu_mlc_load_program: %u bytes", (unsigned)blob_size);
    if (!ucf_blob || blob_size == 0) return ESP_ERR_INVALID_ARG;
    // TODO: parse UCF as register-write tuples, apply via i2c_bus_write
    return ESP_OK;
}

esp_err_t imu_mlc_register_callback(imu_mlc_callback_t callback, void *user_data)
{
    (void)callback; (void)user_data;
    // TODO: store callback, invoke from INT1 ISR-deferred task
    return ESP_OK;
}

esp_err_t imu_mlc_set_enabled(bool enabled)
{
    ESP_LOGI(TAG, "imu_mlc_set_enabled: %d", enabled);
    return ESP_OK;
}

esp_err_t imu_set_wake_enabled(bool enabled)
{
    (void)enabled;
    return ESP_OK;
}

esp_err_t imu_enter_low_power(void) { return ESP_OK; }
esp_err_t imu_exit_low_power(void)  { return ESP_OK; }
