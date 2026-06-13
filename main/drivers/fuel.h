#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Snapshot of battery state from the MAX17048 fuel gauge.
 *
 * `charge` is derived from the gauge's CRATE register (signed %/hr) — the
 * board has no charger STAT line, so charge direction comes from the gauge
 * itself observing the cell.
 */
typedef enum {
    FUEL_CHARGE_UNKNOWN = 0,
    FUEL_CHARGE_CHARGING,
    FUEL_CHARGE_DISCHARGING,
    FUEL_CHARGE_FULL,
    FUEL_CHARGE_CRITICAL,
} fuel_charge_state_t;

typedef struct {
    uint16_t            voltage_mv;    ///< cell voltage, millivolts
    uint8_t             soc_percent;   ///< state of charge, 0..100 (clamped)
    fuel_charge_state_t charge;
    bool                alert_active;  ///< FUEL_ALRT_N pin asserted (active low)
} fuel_status_t;

esp_err_t fuel_init(void);

/** Read a battery snapshot. ESP_ERR_INVALID_STATE if init failed/absent. */
esp_err_t fuel_read(fuel_status_t *out);

#ifdef __cplusplus
}
#endif
