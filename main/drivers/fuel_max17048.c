/**
 * @file fuel_max17048.c
 * @brief MAX17048 fuel gauge driver.
 *
 * Shared between Rev 6 and Rev 7 — same part on both boards. Register
 * conversions graduate from the bring-up sketch's bench-verified `cmd_fuel`
 * (bringup_rev6.c): VCELL/SOC/VERSION confirmed on Rev 6 silicon.
 *
 * CITE: MAX17048 datasheet (Analog Devices/Maxim, 19-6498) register map:
 *   0x02 VCELL   — 16-bit BE; 12-bit left-aligned; 1.25 mV/LSB after >> 4
 *   0x04 SOC     — high byte = % integer, low byte = % fraction / 256
 *   0x08 VERSION — silicon version (presence probe)
 *   0x16 CRATE   — signed 16-bit BE; 0.208 %/hr per LSB; sign gives charge
 *                  direction (positive = charging). Not exercised by the
 *                  bring-up sketch; datasheet-grounded, bench check pending.
 *
 * Remaining TODO: FUEL_ALRT GPIO interrupt for low-battery wake (the pin is
 * read synchronously for now); VALRT threshold configuration.
 */

#include "drivers/fuel.h"
#include "i2c_bus.h"
#include "board.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "oc.fuel";

#define MAX17048_REG_VCELL    0x02u
#define MAX17048_REG_SOC      0x04u
#define MAX17048_REG_VERSION  0x08u
#define MAX17048_REG_CRATE    0x16u

/* CRATE thresholds in raw LSB (0.208 %/hr each). ±5 LSB ≈ ±1 %/hr dead band
 * so measurement noise at rest doesn't flap between charging/discharging. */
#define CRATE_DEADBAND_LSB    5
#define SOC_FULL_PERCENT      99
#define SOC_CRITICAL_PERCENT  5

static bool s_present = false;

static esp_err_t read_u16_be(uint8_t reg, uint16_t *out)
{
    uint8_t b[2] = {0};
    esp_err_t err = i2c_bus_read(BOARD_FUEL_I2C_ADDR, reg, b, 2);
    if (err == ESP_OK) *out = ((uint16_t)b[0] << 8) | b[1];
    return err;
}

esp_err_t fuel_init(void)
{
    /* Presence probe via VERSION — DNP-aware boot: absence is non-fatal,
     * the driver just reports INVALID_STATE on later reads. */
    uint16_t ver = 0;
    esp_err_t err = read_u16_be(MAX17048_REG_VERSION, &ver);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MAX17048 @ 0x%02X not responding (%s)",
                 BOARD_FUEL_I2C_ADDR, esp_err_to_name(err));
        return err;
    }

    gpio_config_t alrt_cfg = {
        .pin_bit_mask = 1ULL << BOARD_FUEL_ALRT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        /* ALRT is open-drain active-low; enable the internal pull-up so the
         * pin idles high when the gauge isn't asserting. */
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&alrt_cfg);

    s_present = true;
    ESP_LOGI(TAG, "MAX17048 @ 0x%02X online, VERSION=0x%04X",
             BOARD_FUEL_I2C_ADDR, ver);
    return ESP_OK;
}

esp_err_t fuel_read(fuel_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_present) return ESP_ERR_INVALID_STATE;

    uint16_t vraw = 0, soc_raw = 0, crate_raw = 0;
    esp_err_t err = read_u16_be(MAX17048_REG_VCELL, &vraw);
    if (err != ESP_OK) return err;
    err = read_u16_be(MAX17048_REG_SOC, &soc_raw);
    if (err != ESP_OK) return err;
    err = read_u16_be(MAX17048_REG_CRATE, &crate_raw);
    if (err != ESP_OK) return err;

    /* 1.25 mV/LSB after the 4-bit right shift: ×125/100 in integer math. */
    out->voltage_mv = (uint16_t)(((uint32_t)(vraw >> 4) * 125u) / 100u);

    uint16_t soc_int = soc_raw >> 8;
    out->soc_percent = (uint8_t)(soc_int > 100 ? 100 : soc_int);

    int16_t crate = (int16_t)crate_raw;
    if (out->soc_percent <= SOC_CRITICAL_PERCENT) {
        out->charge = FUEL_CHARGE_CRITICAL;
    } else if (crate > CRATE_DEADBAND_LSB) {
        out->charge = FUEL_CHARGE_CHARGING;
    } else if (crate < -CRATE_DEADBAND_LSB) {
        out->charge = FUEL_CHARGE_DISCHARGING;
    } else if (out->soc_percent >= SOC_FULL_PERCENT) {
        out->charge = FUEL_CHARGE_FULL;
    } else {
        out->charge = FUEL_CHARGE_UNKNOWN;  /* at rest mid-charge: ambiguous */
    }

    out->alert_active = (gpio_get_level(BOARD_FUEL_ALRT_GPIO) == 0);
    return ESP_OK;
}
