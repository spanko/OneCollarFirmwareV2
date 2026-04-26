/**
 * @file gps_neo_m8q.c
 * @brief NEO-M8Q GPS driver (stub).
 *
 * Shared between Rev 6 and Rev 7. External 6-pin JST-SH breakout — antenna
 * placement is enclosure-driven, not PCB-driven, so the same driver applies.
 *
 * GPS is the dominant power sink on the device. Aggressive duty-cycling per
 * the gating policy in arch doc §8 is non-negotiable.
 *
 * TODO: UART init, NMEA parsing (or UBX binary), fix-quality monitoring,
 *       power-gating control.
 */

#include "drivers/gps.h"
#include "esp_log.h"
#include "board.h"

static const char *TAG = "gps";

esp_err_t gps_init(void)
{
    ESP_LOGI(TAG, "gps_init: NEO-M8Q on UART%d (TX=GPIO%d RX=GPIO%d, off by default)",
             BOARD_GPS_UART_PORT, BOARD_GPS_UART_TX_GPIO, BOARD_GPS_UART_RX_GPIO);
    return ESP_OK;
}
