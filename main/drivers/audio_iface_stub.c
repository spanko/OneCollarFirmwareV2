/**
 * @file audio_iface_stub.c
 * @brief Stub for boards where BOARD_HAS_AUDIO=0 (Rev 6).
 */

#include "audio_iface.h"
#include <string.h>

esp_err_t audio_init(void)                           { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_deinit(void)                         { return ESP_ERR_NOT_SUPPORTED; }

esp_err_t audio_get_format(audio_format_t *out_format)
{
    if (out_format) memset(out_format, 0, sizeof(*out_format));
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_read(int16_t *out_buffer, size_t buffer_capacity_samples,
                     uint32_t timeout_ms, size_t *out_samples_read)
{
    (void)out_buffer; (void)buffer_capacity_samples; (void)timeout_ms;
    if (out_samples_read) *out_samples_read = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_set_enabled(bool enabled)            { (void)enabled; return ESP_ERR_NOT_SUPPORTED; }
