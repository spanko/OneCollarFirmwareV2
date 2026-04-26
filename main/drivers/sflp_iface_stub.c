/**
 * @file sflp_iface_stub.c
 * @brief Stub implementation of sflp_iface for boards where BOARD_HAS_SFLP=0.
 *
 * On Rev 6, SFLP is not available on the LSM6DSO32X. Tier 1 code on Rev 6 is
 * expected to host-compute gravity and orientation features from raw accel
 * data; calling sflp_read on Rev 6 returns ESP_ERR_NOT_SUPPORTED to make the
 * absence explicit.
 *
 * This file is conditionally compiled — see main/CMakeLists.txt.
 */

#include "sflp_iface.h"
#include <string.h>

esp_err_t sflp_init(void)                          { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sflp_deinit(void)                        { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sflp_set_enabled(bool enabled)           { (void)enabled; return ESP_ERR_NOT_SUPPORTED; }

esp_err_t sflp_read(sflp_sample_t *out_sample)
{
    if (out_sample) memset(out_sample, 0, sizeof(*out_sample));
    return ESP_ERR_NOT_SUPPORTED;
}
