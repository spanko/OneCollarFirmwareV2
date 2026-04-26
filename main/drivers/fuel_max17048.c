/**
 * @file fuel_max17048.c
 * @brief MAX17048 fuel gauge driver (stub).
 *
 * Shared between Rev 6 and Rev 7 — same part on both boards.
 *
 * TODO: vendor the MAX17048 register-level driver; periodic SOC poll;
 *       FUEL_ALRT GPIO interrupt for low-battery wake.
 */

#include "i2c_bus.h"
#include "board.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "fuel";

esp_err_t fuel_init(void)
{
    ESP_LOGI(TAG, "fuel_init: MAX17048 @ I2C 0x%02X", BOARD_FUEL_I2C_ADDR);
    return ESP_OK;
}
