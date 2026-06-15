#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_service_init(void);

/** True while an IMU stream (Stage B) is active and draining the sampler ring. */
bool ble_service_is_streaming(void);

#ifdef __cplusplus
}
#endif
