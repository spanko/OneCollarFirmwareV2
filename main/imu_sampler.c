/**
 * @file imu_sampler.c
 * @brief Continuous IMU sampler (Stage A). See imu_sampler.h.
 */
#include "imu_sampler.h"
#include "board.h"
#include "hal/imu_iface.h"

#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_task_wdt.h"

#include <string.h>

static const char *TAG = "oc.imu.sampler";

// Ring buffer: NOSPLIT holds whole imu_sample_t items. 256 samples ≈ 2.5 s of
// headroom at 104 Hz before a stalled consumer back-pressures the sampler.
#define RB_ITEM_COUNT   256
#define RB_BYTES        (RB_ITEM_COUNT * sizeof(imu_sample_t))
// Gap threshold for inferring a dropped sample: > 1.8× the nominal period.
#define GAP_DROP_FACTOR_NUM   18
#define GAP_DROP_FACTOR_DEN   10

static TaskHandle_t        s_task   = NULL;
static RingbufHandle_t  s_rb     = NULL;
static volatile bool       s_run    = false;
static uint64_t            s_exp_dt_us = 0;          // nominal inter-sample period
static imu_sampler_stats_t s_stats;
static portMUX_TYPE        s_stats_mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR int2_isr(void *arg)
{
    (void)arg;
    BaseType_t hpw = pdFALSE;
    if (s_task) {
        vTaskNotifyGiveFromISR(s_task, &hpw);
        if (hpw) portYIELD_FROM_ISR();
    }
}

static void account_sample(const imu_sample_t *s)
{
    taskENTER_CRITICAL(&s_stats_mux);
    if (s_stats.samples == 0) {
        s_stats.start_us = s->timestamp_us;
    } else {
        if (s->timestamp_us < s_stats.last_us) {
            s_stats.nonmonotonic = true;
        } else {
            uint64_t gap = s->timestamp_us - s_stats.last_us;
            s_stats.last_gap_us = gap;
            if (gap > s_stats.max_gap_us) s_stats.max_gap_us = gap;
            // Infer missed samples when the gap exceeds 1.8× nominal.
            if (s_exp_dt_us && gap > s_exp_dt_us * GAP_DROP_FACTOR_NUM / GAP_DROP_FACTOR_DEN) {
                s_stats.dropped_est += (gap / s_exp_dt_us) - 1;
            }
        }
    }
    s_stats.last_us = s->timestamp_us;
    s_stats.samples++;
    taskEXIT_CRITICAL(&s_stats_mux);
}

static void sampler_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    while (s_run) {
        // Wake on INT2 data-ready; timeout lets us re-check s_run and feed the WDT.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        esp_task_wdt_reset();
        // Drain every fresh sample (handles a coalesced/edge-missed interrupt).
        imu_sample_t s;
        while (imu_read_sample(&s) == ESP_OK) {
            account_sample(&s);
            if (xRingbufferSend(s_rb, &s, sizeof(s), 0) != pdTRUE) {
                taskENTER_CRITICAL(&s_stats_mux);
                s_stats.ringbuf_full++;
                taskEXIT_CRITICAL(&s_stats_mux);
            }
        }
    }
    vTaskDelete(NULL);
}

esp_err_t imu_sampler_start(void)
{
    if (s_task) return ESP_ERR_INVALID_STATE;

    imu_info_t info;
    uint16_t odr = (imu_get_info(&info) == ESP_OK && info.odr_hz) ? info.odr_hz : 104;
    s_exp_dt_us = 1000000ULL / odr;

    memset(&s_stats, 0, sizeof(s_stats));
    s_rb = xRingbufferCreate(RB_BYTES, RINGBUF_TYPE_NOSPLIT);
    if (!s_rb) {
        ESP_LOGE(TAG, "ring buffer alloc failed (%u bytes)", (unsigned)RB_BYTES);
        return ESP_ERR_NO_MEM;
    }

    // INT2 GPIO: input, rising-edge (DRDY is active-high push-pull).
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_IMU_INT2_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,   // defined level if the IMU isn't driving
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) { vRingbufferDelete(s_rb); s_rb = NULL; return err; }

    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // already installed is fine
        vRingbufferDelete(s_rb); s_rb = NULL; return err;
    }
    err = gpio_isr_handler_add(BOARD_IMU_INT2_GPIO, int2_isr, NULL);
    if (err != ESP_OK) { vRingbufferDelete(s_rb); s_rb = NULL; return err; }

    s_run = true;
    // Core 0, prio 8, 4 KB — IMU sample / Tier 0 dispatch task (conventions §FreeRTOS).
    BaseType_t ok = xTaskCreatePinnedToCore(sampler_task, "imu_sampler", 4096,
                                            NULL, 8, &s_task, 0);
    if (ok != pdPASS) {
        s_run = false;
        gpio_isr_handler_remove(BOARD_IMU_INT2_GPIO);
        vRingbufferDelete(s_rb); s_rb = NULL; s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "sampler started: %u Hz, nominal dt=%llu us, rb=%u samples",
             odr, (unsigned long long)s_exp_dt_us, RB_ITEM_COUNT);
    return ESP_OK;
}

esp_err_t imu_sampler_stop(void)
{
    if (!s_task) return ESP_ERR_INVALID_STATE;
    s_run = false;
    gpio_isr_handler_remove(BOARD_IMU_INT2_GPIO);
    xTaskNotifyGive(s_task);          // unblock so the task can exit
    s_task = NULL;
    // The task deletes itself; the ring buffer is left for in-flight consumers
    // to drain and is freed on the next start (memset above). Keep it simple.
    return ESP_OK;
}

RingbufHandle_t imu_sampler_ringbuf(void) { return s_rb; }

void imu_sampler_get_stats(imu_sampler_stats_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_stats_mux);
    *out = s_stats;
    taskEXIT_CRITICAL(&s_stats_mux);
}
