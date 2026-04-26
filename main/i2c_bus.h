/**
 * @file i2c_bus.h
 * @brief Shared I2C bus with mutex protection.
 *
 * Three peripherals share the bus on both revs (IMU, fuel gauge, baro).
 * All bus access goes through this driver; per-peripheral drivers must not
 * call ESP-IDF i2c_master_* APIs directly.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2c_bus_init(void);
esp_err_t i2c_bus_deinit(void);

esp_err_t i2c_bus_read(uint8_t device_addr,
                       uint8_t reg_addr,
                       uint8_t *out_buffer,
                       size_t length);

esp_err_t i2c_bus_write(uint8_t device_addr,
                        uint8_t reg_addr,
                        const uint8_t *buffer,
                        size_t length);

#ifdef __cplusplus
}
#endif
