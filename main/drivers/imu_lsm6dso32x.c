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
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "oc.imu";

// Register map + init values graduated from the bench-verified bring-up
// sequence (bringup_rev6.c cmd_imu_init / cmd_imu_read_once), confirmed on
// real Rev 6 silicon. FS_XL encoding on the DSO32X: 00=±4g, 01=±32g, 10=±8g,
// 11=±16g (differs from the plain LSM6DSO — verified against
// STMicroelectronics/lsm6dso32x-pid).
#define LSM6_WHO_AM_I_VALUE   0x6C
#define LSM6_REG_WHO_AM_I     0x0F
#define LSM6_REG_CTRL1_XL     0x10
#define LSM6_REG_CTRL2_G      0x11
#define LSM6_REG_CTRL3_C      0x12
#define LSM6_REG_STATUS       0x1E
#define LSM6_REG_OUTX_L_G     0x22  // gyro X..Z then accel X..Z, 12 bytes, IF_INC auto-increment

#define LSM6_CTRL3_C_INIT     0x44  // BDU=1, IF_INC=1
#define LSM6_CTRL1_XL_INIT    0x40  // 104 Hz, FS_XL=00 -> ±4 g
#define LSM6_CTRL2_G_INIT     0x44  // 104 Hz, FS_G=500 dps
#define LSM6_STATUS_XLDA      0x01  // accel data-ready
#define LSM6_STATUS_GDA       0x02  // gyro data-ready

// Sensitivities for the init ranges above (DSO32X datasheet, bench-confirmed):
//   ±4 g    -> 0.122 mg/LSB  (x122/1000)
//   500 dps -> 17.50 mdps/LSB (x175/10)
// ±4 g suits the Rev 6 interpretable features (gravity-derived posture, gait,
// activity RMS); true impacts are the Rev 7 ±320 g high-g channel's job.
// Widening to ±16 g is a one-line CTRL1_XL change if launch data wants it.
#define ACCEL_MG_NUM   122
#define ACCEL_MG_DEN   1000
#define GYRO_MDPS_NUM  175
#define GYRO_MDPS_DEN  10

static bool s_ready = false;

esp_err_t imu_init(void)
{
    uint8_t who = 0;
    esp_err_t err = i2c_bus_read(BOARD_IMU_I2C_ADDR, LSM6_REG_WHO_AM_I, &who, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LSM6DSO32X @ 0x%02X not responding (%s)",
                 BOARD_IMU_I2C_ADDR, esp_err_to_name(err));
        return err;
    }
    if (who != LSM6_WHO_AM_I_VALUE) {
        ESP_LOGE(TAG, "WHO_AM_I=0x%02X, expected 0x%02X", who, LSM6_WHO_AM_I_VALUE);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t ctrl3 = LSM6_CTRL3_C_INIT;
    uint8_t ctrl1 = LSM6_CTRL1_XL_INIT;
    uint8_t ctrl2 = LSM6_CTRL2_G_INIT;
    if (i2c_bus_write(BOARD_IMU_I2C_ADDR, LSM6_REG_CTRL3_C, &ctrl3, 1) != ESP_OK ||
        i2c_bus_write(BOARD_IMU_I2C_ADDR, LSM6_REG_CTRL1_XL, &ctrl1, 1) != ESP_OK ||
        i2c_bus_write(BOARD_IMU_I2C_ADDR, LSM6_REG_CTRL2_G, &ctrl2, 1) != ESP_OK) {
        ESP_LOGE(TAG, "CTRL register config failed");
        return ESP_FAIL;
    }

    s_ready = true;
    ESP_LOGI(TAG, "LSM6DSO32X online @ 0x%02X (WHO_AM_I=0x%02X, 104 Hz, ±4 g / 500 dps)",
             BOARD_IMU_I2C_ADDR, who);
    // TODO: FIFO + INT1 MLC routing for Tier 0 wake (separate task).
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
    out_info->who_am_i = LSM6_WHO_AM_I_VALUE;
    out_info->mlc_tree_count = 8;
    out_info->mlc_node_count = 256;
    out_info->odr_hz = 104;
    return ESP_OK;
}

esp_err_t imu_read_sample(imu_sample_t *out_sample)
{
    if (!out_sample) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    uint8_t status = 0;
    esp_err_t err = i2c_bus_read(BOARD_IMU_I2C_ADDR, LSM6_REG_STATUS, &status, 1);
    if (err != ESP_OK) return err;
    if (!(status & (LSM6_STATUS_XLDA | LSM6_STATUS_GDA))) {
        return ESP_ERR_NOT_FOUND;  // no fresh sample this poll
    }

    // 12-byte burst: gyro XYZ then accel XYZ, little-endian int16
    // (IF_INC=1 auto-increments the register pointer across the read).
    uint8_t b[12];
    err = i2c_bus_read(BOARD_IMU_I2C_ADDR, LSM6_REG_OUTX_L_G, b, sizeof(b));
    if (err != ESP_OK) return err;

    int16_t gx = (int16_t)((uint16_t)b[0]  | ((uint16_t)b[1]  << 8));
    int16_t gy = (int16_t)((uint16_t)b[2]  | ((uint16_t)b[3]  << 8));
    int16_t gz = (int16_t)((uint16_t)b[4]  | ((uint16_t)b[5]  << 8));
    int16_t ax = (int16_t)((uint16_t)b[6]  | ((uint16_t)b[7]  << 8));
    int16_t ay = (int16_t)((uint16_t)b[8]  | ((uint16_t)b[9]  << 8));
    int16_t az = (int16_t)((uint16_t)b[10] | ((uint16_t)b[11] << 8));

    memset(out_sample, 0, sizeof(*out_sample));
    out_sample->timestamp_us = (uint64_t)esp_timer_get_time();
    out_sample->accel_x_mg = (int32_t)ax * ACCEL_MG_NUM / ACCEL_MG_DEN;
    out_sample->accel_y_mg = (int32_t)ay * ACCEL_MG_NUM / ACCEL_MG_DEN;
    out_sample->accel_z_mg = (int32_t)az * ACCEL_MG_NUM / ACCEL_MG_DEN;
    out_sample->gyro_x_mdps = (int32_t)gx * GYRO_MDPS_NUM / GYRO_MDPS_DEN;
    out_sample->gyro_y_mdps = (int32_t)gy * GYRO_MDPS_NUM / GYRO_MDPS_DEN;
    out_sample->gyro_z_mdps = (int32_t)gz * GYRO_MDPS_NUM / GYRO_MDPS_DEN;
    out_sample->flags = IMU_SAMPLE_FLAG_NONE;
    return ESP_OK;
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
