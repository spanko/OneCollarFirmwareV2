/**
 * @file main.c
 * @brief OneCollar firmware boot sequence.
 *
 * Rev-agnostic. All hardware access goes through HAL ifaces (imu_iface.h,
 * sflp_iface.h, highg_iface.h, audio_iface.h) whose backends are selected at
 * compile time by main/CMakeLists.txt based on the BOARD variable.
 *
 * Failure policy:
 *   - IMU failure is fatal — without IMU there is no Tier 0, no Tier 1, no
 *     reason for the device to be running. Park in an idle loop and log;
 *     do NOT silently reboot — that hides the problem from the developer.
 *   - SFLP / high-g / audio failures are non-fatal. On Rev 6 they return
 *     ESP_ERR_NOT_SUPPORTED by design (capability stubs); on Rev 7 they may
 *     return real errors which we log and continue without.
 *   - Fuel gauge, GPS, LoRa failures are non-fatal — log and continue.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "board.h"
#include "i2c_bus.h"
#include "data_logger.h"
#include "imu_sampler.h"

#include "hal/imu_iface.h"
#include "hal/sflp_iface.h"
#include "hal/highg_iface.h"
#include "hal/audio_iface.h"

#include "drivers/fuel.h"
#include "drivers/ble_service.h"
#include "drivers/lora.h"
#include "drivers/gps.h"
#include "drivers/power.h"

static const char *TAG = "main";

// ---------------------------------------------------------------------------
// Banner
// ---------------------------------------------------------------------------
static void log_banner(void)
{
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " OneCollar firmware — %s (HW rev %d)", BOARD_NAME, BOARD_HW_REV);
    ESP_LOGI(TAG, " Build target: ESP32-S3");
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "Capabilities:");
    ESP_LOGI(TAG, "  IMU (low-g):  %s", BOARD_HAS_IMU_LOWG ? "yes" : "no");
    ESP_LOGI(TAG, "  IMU (high-g): %s", BOARD_HAS_IMU_HIGHG ? "yes" : "no (rev7+)");
    ESP_LOGI(TAG, "  SFLP fusion:  %s", BOARD_HAS_IMU_SFLP ? "yes (chip-native)" : "no (host-computed)");
    ESP_LOGI(TAG, "  MLC:          %s", BOARD_HAS_IMU_MLC ? "yes (8 trees / 256 nodes)" : "no");
    ESP_LOGI(TAG, "  Audio:        %s", BOARD_HAS_AUDIO ? "yes (I2S MEMS)" : "no (rev7+)");
    ESP_LOGI(TAG, "  GPS:          %s", BOARD_HAS_GPS ? "yes" : "no");
    ESP_LOGI(TAG, "  LoRa:         %s", BOARD_HAS_LORA ? "yes" : "no");
    ESP_LOGI(TAG, "  Sub-GHz:      %s", BOARD_HAS_SUBGHZ_RADIO ? "yes (CC1101)" : "no (removed in rev7)");
}

// ---------------------------------------------------------------------------
// IMU failure handler — park, don't reboot
// ---------------------------------------------------------------------------
static void imu_fatal_park(esp_err_t err)
{
    ESP_LOGE(TAG, "FATAL: IMU init failed (%s). Device is unusable; parking.",
             esp_err_to_name(err));
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGE(TAG, "IMU FAULT — power-cycle to retry");
    }
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
void app_main(void)
{
    log_banner();

    // 1. NVS (config, geofences, per-dog adaptation weights)
    {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "NVS partition needs erase, re-initializing");
            ESP_ERROR_CHECK(nvs_flash_erase());
            ESP_ERROR_CHECK(nvs_flash_init());
        } else {
            ESP_ERROR_CHECK(err);
        }
    }

    // 2. I2C bus (must come before IMU / fuel / baro)
    ESP_ERROR_CHECK(i2c_bus_init());

    // 3. IMU — fatal if absent
    {
        esp_err_t err = imu_init();
        if (err != ESP_OK) {
            imu_fatal_park(err);
        }
        imu_info_t info;
        if (imu_get_info(&info) == ESP_OK) {
            ESP_LOGI(TAG, "IMU online: %s (whoami=0x%02X, %u trees / %u nodes, %u Hz)",
                     info.part_name, info.who_am_i,
                     info.mlc_tree_count, info.mlc_node_count, info.odr_hz);
        }
    }

    // 3b. Continuous IMU sampler (Stage A, docs/07a). Always-on for now so the
    //     bench can validate acquisition (Gate A); production will gate it on an
    //     active capture (Stage B). Non-fatal: a sampler failure leaves the
    //     collar otherwise functional.
    if (imu_sampler_start() != ESP_OK) {
        ESP_LOGW(TAG, "imu_sampler_start failed — capture unavailable");
    }

    // 4. SFLP — Rev 7 capability; stub returns NOT_SUPPORTED on Rev 6
    {
        esp_err_t err = sflp_init();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "SFLP online (chip-native gravity / quaternion)");
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGI(TAG, "SFLP not present — gravity will be host-computed");
        } else {
            ESP_LOGW(TAG, "SFLP init failed (%s) — falling back to host-computed", esp_err_to_name(err));
        }
    }

    // 5. High-g — Rev 7 capability
    {
        esp_err_t err = highg_init();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "High-g channel online (impact characterization available)");
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGI(TAG, "High-g not present — impact saturates above ±32 g");
        } else {
            ESP_LOGW(TAG, "High-g init failed: %s", esp_err_to_name(err));
        }
    }

    // 6. Audio — Rev 7 capability
    {
        esp_err_t err = audio_init();
        if (err == ESP_OK) {
            audio_format_t fmt;
            audio_get_format(&fmt);
            ESP_LOGI(TAG, "Audio online: %u Hz / %u-bit / %u ch",
                     fmt.sample_rate_hz, fmt.bits_per_sample, fmt.channels);
        } else if (err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGI(TAG, "Audio not present — Tier 2 vocal behaviors disabled");
        } else {
            ESP_LOGW(TAG, "Audio init failed: %s", esp_err_to_name(err));
        }
    }

    // 7. Power / charger / fuel gauge — non-fatal
    if (power_init()       != ESP_OK) ESP_LOGW(TAG, "power_init failed");
    if (fuel_init()        != ESP_OK) ESP_LOGW(TAG, "fuel_init failed");

    // 8. Radios — non-fatal
    if (ble_service_init() != ESP_OK) ESP_LOGW(TAG, "ble_service_init failed");
#if BOARD_HAS_LORA
    if (lora_init()        != ESP_OK) ESP_LOGW(TAG, "lora_init failed");
#endif
#if BOARD_HAS_GPS
    if (gps_init()         != ESP_OK) ESP_LOGW(TAG, "gps_init failed (off by default — gated by location policy)");
#endif

    // 9. Data logger — mount the sessions filesystem. Capture is now explicit
    //    (BLE StartCapture, Stage C); no boot-auto-session (it accumulated a
    //    header-only file per boot).
    if (data_logger_init() != ESP_OK) {
        ESP_LOGW(TAG, "data_logger_init failed — flash capture unavailable");
    }

    ESP_LOGI(TAG, "Boot complete. Tier 0 will arm once MLC program is loaded.");

    // 10. Idle loop. Real firmware spawns Tier-1 / data-pump consumers; for now
    //     this drains the sampler ring buffer (so it doesn't back-pressure) and
    //     reports Stage A / Gate A acquisition stats.
    RingbufHandle_t rb = imu_sampler_ringbuf();
    int tick = 0;
    while (1) {
        // Drain every 1 s so the 256-deep buffer never back-pressures (Gate A
        // has no real consumer yet; Stage B/C replace this discard). Log stats
        // every 5 s.
        vTaskDelay(pdMS_TO_TICKS(1000));
        // Drain + discard ONLY when no real consumer is active — while an IMU
        // stream (Stage B) runs, its task is the sole ring consumer; two readers
        // would split the sample stream.
        if (rb && !ble_service_is_streaming() && !ble_service_is_capturing()) {
            size_t sz;
            void *item;
            while ((item = xRingbufferReceive(rb, &sz, 0)) != NULL) {
                vRingbufferReturnItem(rb, item);
            }
        }
        if (++tick < 5) continue;
        tick = 0;

        imu_sampler_stats_t st;
        imu_sampler_get_stats(&st);
        if (st.samples > 1 && st.last_us > st.start_us) {
            double secs = (double)(st.last_us - st.start_us) / 1e6;
            double rate = (double)(st.samples - 1) / secs;
            ESP_LOGI(TAG, "Gate A: %llu samples, %.2f Hz, dropped~%llu, max_gap=%llu us, rb_full=%u%s | FS:%s",
                     (unsigned long long)st.samples, rate,
                     (unsigned long long)st.dropped_est,
                     (unsigned long long)st.max_gap_us, st.ringbuf_full,
                     st.nonmonotonic ? ", NONMONOTONIC!" : "",
                     data_logger_fs_ready() ? "mounted" : "DOWN");
        } else {
            ESP_LOGW(TAG, "Gate A: no samples yet (caps=0x%08x)",
                     data_logger_current_capabilities());
        }
    }
}
