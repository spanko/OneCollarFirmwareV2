/**
 * @file highg_iface.h
 * @brief High-g accelerometer channel — Rev 7 only.
 *
 * The LSM6DSV320X has a dedicated second accelerometer with ±320 g full-scale
 * and its own FIFO tag, separate from the low-g UI channel. This enables
 * impact characterization (vehicle strike, fall, severe play collision) that
 * a single ±32 g sensor would saturate on.
 *
 * On Rev 6 (LSM6DSO32X — single accel channel saturates at ±32 g), all
 * functions return ESP_ERR_NOT_SUPPORTED. The data logger tags sessions with
 * DATALOG_CAP_IMU_HIGHG so impact-detection model training can require it.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** High-g sample. Note larger range than imu_sample_t.accel_*_mg. */
typedef struct {
    uint64_t timestamp_us;
    int16_t  accel_x_g;        ///< g (1 g = 9.80665 m/s^2). Range up to ±320.
    int16_t  accel_y_g;
    int16_t  accel_z_g;
    int16_t  peak_g;           ///< chip-tracked peak magnitude over recent window
} highg_sample_t;

/** Configurable peak-detect threshold for waking Tier 1. */
typedef struct {
    int16_t threshold_g;       ///< trigger if any axis exceeds this magnitude
    uint16_t window_ms;        ///< observation window for peak tracking
} highg_peak_config_t;

typedef void (*highg_peak_callback_t)(const highg_sample_t *sample, void *user_data);

esp_err_t highg_init(void);
esp_err_t highg_deinit(void);
esp_err_t highg_set_enabled(bool enabled);

esp_err_t highg_read(highg_sample_t *out_sample);
esp_err_t highg_set_peak_config(const highg_peak_config_t *config);
esp_err_t highg_register_peak_callback(highg_peak_callback_t cb, void *user_data);

#ifdef __cplusplus
}
#endif
