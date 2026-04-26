/**
 * @file i2c_bus.c
 * @brief Shared I2C bus implementation (stub).
 *
 * TODO: Implement using ESP-IDF i2c_master driver (new-style, not legacy).
 *       Bus configuration sourced from board.h: BOARD_I2C_PORT, BOARD_I2C_SDA_GPIO,
 *       BOARD_I2C_SCL_GPIO, BOARD_I2C_FREQ_HZ.
 *       Mutex must be FreeRTOS recursive — drivers may nest read-modify-write.
 */

#include "i2c_bus.h"
#include "board.h"
#include "esp_log.h"

static const char *TAG = "i2c_bus";

esp_err_t i2c_bus_init(void)
{
    ESP_LOGI(TAG, "i2c_bus_init: port=%d sda=%d scl=%d freq=%d",
             BOARD_I2C_PORT, BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO, BOARD_I2C_FREQ_HZ);
    // TODO: configure i2c_master_bus, install with mutex
    return ESP_OK;
}

esp_err_t i2c_bus_deinit(void)
{
    // TODO
    return ESP_OK;
}

esp_err_t i2c_bus_read(uint8_t device_addr, uint8_t reg_addr,
                       uint8_t *out_buffer, size_t length)
{
    (void)device_addr; (void)reg_addr; (void)out_buffer; (void)length;
    // TODO: i2c_master_transmit_receive
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t i2c_bus_write(uint8_t device_addr, uint8_t reg_addr,
                        const uint8_t *buffer, size_t length)
{
    (void)device_addr; (void)reg_addr; (void)buffer; (void)length;
    // TODO: i2c_master_transmit
    return ESP_ERR_NOT_SUPPORTED;
}
