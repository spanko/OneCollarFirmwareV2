/**
 * @file i2c_bus.c
 * @brief Shared I2C bus implementation.
 *
 * New-style i2c_master driver (ESP-IDF v6; driver/i2c_master.h — the legacy
 * driver/i2c.h is forbidden by conventions). Bus parameters come from
 * board.h: BOARD_I2C_PORT, BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO,
 * BOARD_I2C_FREQ_HZ. Register sequence graduated from the bring-up sketch's
 * bench-verified i2c_bus_open()/i2c_read_reg()/i2c_write_reg()
 * (bringup_rev6.c) with two production deltas:
 *
 *   - A FreeRTOS recursive mutex serializes transactions. Recursive so a
 *     future i2c_bus_lock()/unlock() pair can wrap multi-transaction
 *     read-modify-write sequences without deadlocking the helpers inside.
 *   - Device handles are cached per address instead of add/remove per call —
 *     the bring-up churns handles deliberately (REPL can re-pin/re-speed the
 *     bus at runtime); production pins are fixed at board.h.
 */

#include <string.h>

#include "i2c_bus.h"
#include "board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "i2c_bus";

#define I2C_TIMEOUT_MS     100
#define I2C_MAX_DEVICES    8     /* IMU, fuel, baro + headroom */
#define I2C_WRITE_MAX      32    /* longest reg-write payload we support */

static i2c_master_bus_handle_t s_bus = NULL;
static SemaphoreHandle_t       s_mutex = NULL;

typedef struct {
    uint8_t                  addr;
    i2c_master_dev_handle_t  dev;
} dev_cache_entry_t;

static dev_cache_entry_t s_devs[I2C_MAX_DEVICES];
static size_t            s_dev_count = 0;

static esp_err_t dev_for_addr(uint8_t addr, i2c_master_dev_handle_t *out)
{
    for (size_t i = 0; i < s_dev_count; i++) {
        if (s_devs[i].addr == addr) {
            *out = s_devs[i].dev;
            return ESP_OK;
        }
    }
    if (s_dev_count >= I2C_MAX_DEVICES) return ESP_ERR_NO_MEM;
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = BOARD_I2C_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev;
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dcfg, &dev);
    if (err != ESP_OK) return err;
    s_devs[s_dev_count].addr = addr;
    s_devs[s_dev_count].dev  = dev;
    s_dev_count++;
    *out = dev;
    return ESP_OK;
}

esp_err_t i2c_bus_init(void)
{
    if (s_bus != NULL) return ESP_OK;
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateRecursiveMutex();
        if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    }
    i2c_master_bus_config_t cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = BOARD_I2C_PORT,
        .scl_io_num                   = BOARD_I2C_SCL_GPIO,
        .sda_io_num                   = BOARD_I2C_SDA_GPIO,
        .glitch_ignore_cnt            = 7,
        /* PCB carries 4.7 kΩ pull-ups; internal ones are belt-and-suspenders
         * and harmless alongside them (same as the bring-up config). */
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus -> %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "up: port=%d sda=%d scl=%d freq=%d",
             BOARD_I2C_PORT, BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO,
             BOARD_I2C_FREQ_HZ);
    return ESP_OK;
}

esp_err_t i2c_bus_deinit(void)
{
    if (s_bus == NULL) return ESP_OK;
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_dev_count; i++) {
        i2c_master_bus_rm_device(s_devs[i].dev);
    }
    s_dev_count = 0;
    esp_err_t err = i2c_del_master_bus(s_bus);
    s_bus = NULL;
    xSemaphoreGiveRecursive(s_mutex);
    return err;
}

esp_err_t i2c_bus_read(uint8_t device_addr, uint8_t reg_addr,
                       uint8_t *out_buffer, size_t length)
{
    if (s_bus == NULL) return ESP_ERR_INVALID_STATE;
    if (out_buffer == NULL || length == 0) return ESP_ERR_INVALID_ARG;
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    i2c_master_dev_handle_t dev;
    esp_err_t err = dev_for_addr(device_addr, &dev);
    if (err == ESP_OK) {
        err = i2c_master_transmit_receive(dev, &reg_addr, 1,
                                          out_buffer, length, I2C_TIMEOUT_MS);
    }
    xSemaphoreGiveRecursive(s_mutex);
    return err;
}

esp_err_t i2c_bus_write(uint8_t device_addr, uint8_t reg_addr,
                        const uint8_t *buffer, size_t length)
{
    if (s_bus == NULL) return ESP_ERR_INVALID_STATE;
    if (length > I2C_WRITE_MAX) return ESP_ERR_INVALID_ARG;
    uint8_t frame[1 + I2C_WRITE_MAX];
    frame[0] = reg_addr;
    if (length > 0) {
        if (buffer == NULL) return ESP_ERR_INVALID_ARG;
        memcpy(&frame[1], buffer, length);
    }
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
    i2c_master_dev_handle_t dev;
    esp_err_t err = dev_for_addr(device_addr, &dev);
    if (err == ESP_OK) {
        err = i2c_master_transmit(dev, frame, 1 + length, I2C_TIMEOUT_MS);
    }
    xSemaphoreGiveRecursive(s_mutex);
    return err;
}
