/**
 * @file power.c
 * @brief Power management (stub).
 *
 * Shared. BQ25185 CE (CHGR-EN-N, active-low) drive, deep-sleep coordination,
 * voltage rail control if we add per-rail enables in a future rev.
 *
 * TODO: esp_pm_configure, deep-sleep wake sources
 *       (IMU INT1 for activity, BLE for connection, optional VAD on Rev 7).
 */

#include "drivers/power.h"
#include "esp_log.h"
#include "board.h"

static const char *TAG = "power";

esp_err_t power_init(void)
{
    ESP_LOGI(TAG, "power_init: battery=%dmAh chemistry=%s charger CE GPIO%d (active low)",
             BOARD_BATTERY_MAH, BOARD_BATTERY_CHEMISTRY, BOARD_CHARGER_EN_N_GPIO);
    return ESP_OK;
}
