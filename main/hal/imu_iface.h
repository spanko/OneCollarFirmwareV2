/**
 * @file imu_iface.h
 * @brief Hardware-agnostic IMU interface.
 *
 * Implemented by per-rev driver backends (drivers/imu_lsm6dso32x.c on Rev 6,
 * drivers/imu_lsm6dsv320x.c on Rev 7). Both parts are ST LSM6 family with the
 * same MLC capacity (8 trees, 256 nodes), so the interface is genuinely common.
 *
 * Capabilities that exist only on Rev 7 — SFLP fusion output, high-g channel —
 * live in their own iface headers (sflp_iface.h, highg_iface.h) and are gated
 * by BOARD_HAS_SFLP / BOARD_HAS_IMU_HIGHG. This keeps shared code shared and
 * makes the rev-specific deltas explicit.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Normalized IMU sample. Units are fixed across revs so Tier 1+ code does not
 * need to know which chip produced the sample. Drivers are responsible for
 * converting from chip-native LSB units into the normalized form.
 */
typedef struct {
    uint64_t timestamp_us;     ///< Capture time (board epoch, microseconds).
    int32_t  accel_x_mg;       ///< milli-g (1 mg = 9.80665 mm/s^2).
    int32_t  accel_y_mg;
    int32_t  accel_z_mg;
    int32_t  gyro_x_mdps;      ///< milli-degrees per second.
    int32_t  gyro_y_mdps;
    int32_t  gyro_z_mdps;
    uint16_t flags;            ///< IMU_SAMPLE_FLAG_* bitfield.
} imu_sample_t;
/* Fields are int32: at 500 dps full-scale a gyro reading reaches ~573 dps =
 * 573000 mdps, far past int16's ±32767. Matches the SINT32 fields in the BLE
 * ImuSnapshot proto so the value passes to the app without clamping. */

#define IMU_SAMPLE_FLAG_NONE              0x0000
#define IMU_SAMPLE_FLAG_FIFO_OVERFLOW     0x0001
#define IMU_SAMPLE_FLAG_TIMESTAMP_INTERP  0x0002  ///< timestamp is interpolated, not chip-direct

/**
 * MLC decision-tree result. Both DSO32X and DSV320X emit one of these per tree
 * per inference window via INT1.
 */
typedef struct {
    uint64_t timestamp_us;
    uint8_t  tree_id;          ///< 0..7
    uint8_t  class_id;         ///< Tree-defined class (Tier 0 label).
    uint8_t  confidence;       ///< 0..255. DSV320X provides; DSO32X stubs to 255.
    uint8_t  reserved;
} imu_mlc_event_t;

typedef void (*imu_mlc_callback_t)(const imu_mlc_event_t *event, void *user_data);

/** Driver-reported chip identification. Useful for data-logger session metadata. */
typedef struct {
    const char *part_name;     ///< e.g. "LSM6DSO32X" or "LSM6DSV320X"
    uint8_t     who_am_i;      ///< chip-reported identity register
    uint8_t     mlc_tree_count;
    uint16_t    mlc_node_count;
    uint16_t    odr_hz;        ///< current output data rate
} imu_info_t;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
esp_err_t imu_init(void);
esp_err_t imu_deinit(void);
esp_err_t imu_get_info(imu_info_t *out_info);

// ---------------------------------------------------------------------------
// Sample acquisition
// ---------------------------------------------------------------------------

/** Read most-recent sample. Non-blocking. Returns ESP_ERR_NOT_FOUND if no fresh data. */
esp_err_t imu_read_sample(imu_sample_t *out_sample);

/**
 * Drain on-chip FIFO into caller buffer. Returns count actually drained.
 * Both DSO32X and DSV320X support hardware FIFO with configurable watermark.
 */
esp_err_t imu_drain_fifo(imu_sample_t *out_buffer,
                         size_t buffer_capacity,
                         size_t *out_count);

/** Configure output data rate. Common values: 12.5, 26, 52, 104, 208, 416, 833 Hz. */
esp_err_t imu_set_odr_hz(uint16_t odr_hz);

// ---------------------------------------------------------------------------
// Machine Learning Core (Tier 0)
// ---------------------------------------------------------------------------

/**
 * Load MLC program(s) from a UCF blob.
 *
 * UCF (Unico Configuration File) is the artifact format produced by both
 * Unico-GUI (DSO32X / Rev 6 toolchain) and MEMS Studio (DSV320X / Rev 7
 * toolchain). Same on-the-wire format; the driver does not need to know which
 * tool authored the blob.
 *
 * The blob includes register writes that configure the MLC trees, the FSM
 * (where applicable), and any required ODR / FIFO setup. Calling this replaces
 * any previously-loaded program.
 */
esp_err_t imu_mlc_load_program(const uint8_t *ucf_blob, size_t blob_size);

esp_err_t imu_mlc_register_callback(imu_mlc_callback_t callback, void *user_data);
esp_err_t imu_mlc_set_enabled(bool enabled);

// ---------------------------------------------------------------------------
// Wake-on-activity / power
// ---------------------------------------------------------------------------
esp_err_t imu_set_wake_enabled(bool enabled);
esp_err_t imu_enter_low_power(void);
esp_err_t imu_exit_low_power(void);

#ifdef __cplusplus
}
#endif
