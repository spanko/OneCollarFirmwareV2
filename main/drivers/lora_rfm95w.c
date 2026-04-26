/**
 * @file lora_rfm95w.c
 * @brief LoRa RFM95W driver (stub).
 *
 * Shared. SPI2 with CS at BOARD_LORA_CS_GPIO. Power-gated by default;
 * woken by the lost-dog policy (see arch doc §8).
 *
 * TODO: SPI initialization, RFM95W register init, TX/RX state machine,
 *       distress-beacon scheduling.
 */

#include "drivers/lora.h"
#include "esp_log.h"
#include "board.h"

static const char *TAG = "lora";

esp_err_t lora_init(void)
{
    ESP_LOGI(TAG, "lora_init: RFM95W on SPI%d, CS GPIO%d",
             BOARD_SPI_HOST, BOARD_LORA_CS_GPIO);
    return ESP_OK;
}
