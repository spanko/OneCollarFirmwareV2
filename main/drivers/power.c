/**
 * @file power.c
 * @brief Power management (stub).
 *
 * Shared. Charger STAT GPIO monitoring, deep-sleep coordination, voltage
 * rail control if we add per-rail enables in a future rev.
 *
 * TODO: charger STAT interrupt, esp_pm_configure, deep-sleep wake sources
 *       (IMU INT1 for activity, BLE for connection, optional VAD on Rev 7).
 */

#include "esp_err.h"
#include "esp_log.h"
#include "board.h"

static const char *TAG = "power";

esp_err_t power_init(void)
{
    ESP_LOGI(TAG, "power_init: battery=%dmAh chemistry=%s charger STAT GPIO%d",
             BOARD_BATTERY_MAH, BOARD_BATTERY_CHEMISTRY, BOARD_CHARGER_STAT_GPIO);
    return ESP_OK;
}
