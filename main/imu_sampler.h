/**
 * @file imu_sampler.h
 * @brief Continuous IMU sampler — Stage A of the capture pipeline.
 *
 * Drives the LSM6DSO32X at its configured ODR (104 Hz Tier 1 baseline) off the
 * INT2 data-ready interrupt, draining each ready sample via imu_read_sample into
 * a FreeRTOS ring buffer for downstream consumers (Stage B BLE streaming,
 * Stage C flash logging). See docs/07a §4 in onecollar-platform.
 *
 * Design (docs/07a Stage A): a dedicated high-priority task waits on an INT2
 * GPIO ISR notification, then drains all fresh samples (re-checking STATUS) so a
 * coalesced edge can't strand a sample. Without an on-chip FIFO this is NOT
 * drop-proof under task starvation (BDU keeps only the latest sample) — the
 * sampler detects and counts such drops via inter-sample timestamp gaps. The
 * FIFO + watermark upgrade (imu_drain_fifo) is the Stage C robustness path.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Acquisition stats for Gate A validation. */
typedef struct {
    uint64_t samples;        ///< total samples acquired since start
    uint64_t dropped_est;    ///< missed samples inferred from timestamp gaps
    uint32_t ringbuf_full;   ///< pushes dropped because no consumer drained (back-pressure)
    uint64_t max_gap_us;     ///< largest observed inter-sample gap
    uint64_t last_gap_us;    ///< most recent inter-sample gap
    uint64_t start_us;       ///< board-epoch time of first sample (0 until first)
    uint64_t last_us;        ///< board-epoch time of most recent sample
    bool     nonmonotonic;   ///< set if a timestamp ever went backwards (must stay false)
} imu_sampler_stats_t;

/**
 * Start continuous sampling. Configures the INT2 GPIO interrupt, allocates the
 * ring buffer, and spawns the sampler task (Core 0, prio 8, per conventions).
 * imu_init() must have run first (it routes accel DRDY to INT2).
 */
esp_err_t imu_sampler_start(void);

/** Stop sampling and release the task + ISR. */
esp_err_t imu_sampler_stop(void);

/** Ring buffer of imu_sample_t for consumers (Stage B/C). NULL until started. */
RingbufHandle_t imu_sampler_ringbuf(void);

/** Snapshot the acquisition stats. */
void imu_sampler_get_stats(imu_sampler_stats_t *out);

#ifdef __cplusplus
}
#endif
