/**
 * @file audio_iface.h
 * @brief I2S MEMS microphone interface — Rev 7 only.
 *
 * Provides a continuous PCM audio stream sourced from the I2S MEMS microphone.
 * Tier 2 (audio behaviors) consumes from this iface; Tier 0 audio gating uses
 * the optional Infineon PDM+VAD path (separate header — TBD).
 *
 * On Rev 6 (no microphone), all functions return ESP_ERR_NOT_SUPPORTED.
 * Sessions are tagged DATALOG_CAP_AUDIO when this iface is active so the
 * training pipeline can build audio-aware models only against compatible data.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sample_rate_hz;   ///< nominal 16000
    uint8_t  bits_per_sample;  ///< nominal 16
    uint8_t  channels;         ///< nominal 1 (mono)
} audio_format_t;

esp_err_t audio_init(void);
esp_err_t audio_deinit(void);
esp_err_t audio_get_format(audio_format_t *out_format);

/**
 * Read PCM samples from ringbuffer. Blocks up to `timeout_ms` waiting for data.
 * Returns count actually read in *out_samples_read.
 */
esp_err_t audio_read(int16_t *out_buffer,
                     size_t buffer_capacity_samples,
                     uint32_t timeout_ms,
                     size_t *out_samples_read);

esp_err_t audio_set_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
