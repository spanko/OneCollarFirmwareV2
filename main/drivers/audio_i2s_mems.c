/**
 * @file audio_i2s_mems.c
 * @brief I2S MEMS microphone driver — Rev 7.
 *
 * Implements audio_iface.h using the ESP-IDF i2s_std driver. Configures a
 * 16 kHz, 16-bit mono receive channel from the MEMS mic on the I2S pins
 * defined in board.h.
 *
 * TODO:
 *   1. i2s_new_channel(I2S_ROLE_MASTER, RX) with std_config from board.h pins.
 *   2. Maintain a ringbuffer drained by audio_read.
 *   3. DC-offset removal at driver layer (MEMS mics have a sustained bias).
 *   4. Optional: integrate with Tier 0 wake from BOARD_HAS_VAD if populated.
 */

#include "audio_iface.h"
#include "board.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "audio_i2s";

esp_err_t audio_init(void)
{
    ESP_LOGI(TAG, "audio_init: I2S port=%d bclk=%d ws=%d din=%d sr=%d",
             BOARD_I2S_PORT, BOARD_I2S_BCLK_GPIO, BOARD_I2S_WS_GPIO,
             BOARD_I2S_DIN_GPIO, BOARD_I2S_SAMPLE_RATE);
    // TODO: i2s_new_channel + i2s_channel_init_std_mode + i2s_channel_enable
    return ESP_OK;
}

esp_err_t audio_deinit(void)                          { return ESP_OK; }

esp_err_t audio_get_format(audio_format_t *out_format)
{
    if (!out_format) return ESP_ERR_INVALID_ARG;
    out_format->sample_rate_hz = BOARD_I2S_SAMPLE_RATE;
    out_format->bits_per_sample = BOARD_I2S_BITS_PER_SAMP;
    out_format->channels = 1;
    return ESP_OK;
}

esp_err_t audio_read(int16_t *out_buffer, size_t buffer_capacity_samples,
                     uint32_t timeout_ms, size_t *out_samples_read)
{
    (void)out_buffer; (void)buffer_capacity_samples; (void)timeout_ms;
    if (out_samples_read) *out_samples_read = 0;
    // TODO: i2s_channel_read with timeout
    return ESP_OK;
}

esp_err_t audio_set_enabled(bool enabled)
{
    ESP_LOGI(TAG, "audio_set_enabled: %d", enabled);
    return ESP_OK;
}
