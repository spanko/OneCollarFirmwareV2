/**
 * @file sflp_iface.h
 * @brief Sensor Fusion Low Power (SFLP) interface — Rev 7 only.
 *
 * SFLP is an on-chip block in the LSM6DSV320X that produces:
 *   - 3D gravity vector (sub-degree static accuracy)
 *   - Game-rotation quaternion (no magnetometer reference)
 *   - Gyroscope bias estimate
 *
 * These outputs feed three of our ten interpretable features directly:
 *   neck pitch, neck roll, orientation stability.
 *
 * On Rev 6 (LSM6DSO32X — no SFLP), all functions return ESP_ERR_NOT_SUPPORTED.
 * Tier 1 code on Rev 6 host-computes the same features from raw accel data;
 * the data logger tags sessions with DATALOG_CAP_IMU_SFLP so the training
 * pipeline can distinguish chip-native from host-derived gravity.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Quaternion in (i, j, k, w) order. Unit length within sensor accuracy. */
typedef struct {
    float i;
    float j;
    float k;
    float w;
} sflp_quaternion_t;

/** Gravity vector in g (gravitational units), board frame. */
typedef struct {
    float x;
    float y;
    float z;
} sflp_gravity_t;

typedef struct {
    uint64_t          timestamp_us;
    sflp_quaternion_t quat;          ///< game-rotation quaternion
    sflp_gravity_t    gravity;       ///< gravity vector
    float             gyro_bias_x_dps;
    float             gyro_bias_y_dps;
    float             gyro_bias_z_dps;
    uint8_t           accuracy;      ///< 0..3 (chip-reported quality)
} sflp_sample_t;

esp_err_t sflp_init(void);
esp_err_t sflp_deinit(void);
esp_err_t sflp_set_enabled(bool enabled);

/** Read most-recent SFLP output. Non-blocking. */
esp_err_t sflp_read(sflp_sample_t *out_sample);

#ifdef __cplusplus
}
#endif
