/**
 * @file ble_service.c
 * @brief BLE service (stub).
 *
 * Shared. Per kickoff brief: clean-sheet design on latest SDKs. v2 reliability
 * layer (SACK, flow control, fragmentation) from the prior MAUI app's
 * EnhancedBluetoothCommunicationService is inspiration; we do not carry the
 * code forward.
 *
 * Counterpart Flutter app pairs by Nordic UART Service UUIDs (preserved from
 * Rev 4 contract for app-side continuity during transition).
 *
 * TODO: NimBLE host init, NUS GATT registration, MTU exchange, packet
 *       framing per the new spec being authored alongside the Flutter app.
 */

#include "drivers/ble_service.h"
#include "esp_log.h"

static const char *TAG = "ble";

esp_err_t ble_service_init(void)
{
    ESP_LOGI(TAG, "ble_service_init (stub)");
    return ESP_OK;
}
