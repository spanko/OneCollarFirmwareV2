/**
 * @file highg_iface_stub.c
 * @brief Stub for boards where BOARD_HAS_IMU_HIGHG=0.
 *
 * Compiled only when the active board lacks a dedicated high-g channel
 * (Rev 6). All entry points return ESP_ERR_NOT_SUPPORTED.
 */

#include "highg_iface.h"
#include <string.h>

esp_err_t highg_init(void)                                { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t highg_deinit(void)                              { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t highg_set_enabled(bool enabled)                 { (void)enabled; return ESP_ERR_NOT_SUPPORTED; }

esp_err_t highg_read(highg_sample_t *out_sample)
{
    if (out_sample) memset(out_sample, 0, sizeof(*out_sample));
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t highg_set_peak_config(const highg_peak_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t highg_register_peak_callback(highg_peak_callback_t cb, void *user_data)
{
    (void)cb; (void)user_data;
    return ESP_ERR_NOT_SUPPORTED;
}
