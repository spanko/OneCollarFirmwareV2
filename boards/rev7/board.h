/**
 * @file board.h
 * @brief OneCollar Rev 7 — pin map and capability flags.
 *
 * Hardware: ESP32-S3-WROOM-1-N16R8 + STMicro LSM6DSV320X (IMU) +
 *           MAX17048 (fuel gauge) + RFM95W (LoRa) + NEO-M8Q (GPS, external) +
 *           I2S MEMS microphone (acoustic vent in enclosure) +
 *           BMP390 (barometer, DNP — re-evaluate per power cost).
 *
 * Differences from Rev 6:
 *   - IMU is LSM6DSV320X (was LSM6DSO32X) — same I2C address, adds SFLP and
 *     high-g channel as parallel on-chip blocks alongside MLC.
 *   - I2S MEMS capture microphone added — Tier 2 audio behaviors. Motion-woken
 *     only (recorded after MLC wakes the device); there is NO always-on acoustic
 *     wake. Motion (MLC) is the sole wake source. (decision log 2026-06-16)
 *   - CC1101 sub-GHz radio removed (no firmware dependency in V1; revisit if
 *     802.15.4 / Thread becomes required). Frees BOARD_CC1101_CS_GPIO.
 *
 * NOTE: Pin assignments below are projected from the Rev 6 baseline plus
 * architectural intent. Patrick owns final Rev 7 PCB pin selection. All pins
 * marked PATRICK_TODO must be confirmed before silicon order.
 */

#pragma once

// ---------------------------------------------------------------------------
// Board identity
// ---------------------------------------------------------------------------
#define BOARD_NAME              "OneCollar Rev 7"
#define BOARD_HW_REV            7
#define BOARD_PCB_DATE          "TBD — Patrick to confirm"

// ---------------------------------------------------------------------------
// Capability flags
// ---------------------------------------------------------------------------
#define BOARD_HAS_IMU           1
#define BOARD_HAS_IMU_LOWG      1   // LSM6DSV320X low-g channel: ±16 g
#define BOARD_HAS_IMU_HIGHG     1   // LSM6DSV320X dedicated high-g channel: ±320 g
#define BOARD_HAS_IMU_SFLP      1   // chip-native gravity vector + game-rotation quaternion
#define BOARD_HAS_IMU_MLC       1   // 8 trees, 256 nodes (same capacity as DSO32X)
#define BOARD_HAS_IMU_FSM       1   // 8 programmable state machines + ASC
#define BOARD_HAS_FUEL_GAUGE    1
#define BOARD_HAS_BAROMETER     0   // BMP390 footprint, DNP — re-evaluate
#define BOARD_HAS_AUDIO         1   // digital I2S/PDM MEMS capture mic, motion-woken
                                    // (part TBD; analog ruled out — see decision log)
#define BOARD_HAS_VAD           0   // DROPPED 2026-06-16: motion (MLC) is the sole wake
                                    // source — no always-on acoustic wake. (Kept as 0,
                                    // not removed, so #if BOARD_HAS_VAD stays valid.)
#define BOARD_HAS_GPS           1
#define BOARD_HAS_LORA          1
#define BOARD_HAS_SUBGHZ_RADIO  0   // CC1101 removed
#define BOARD_HAS_LED_RGB       0
#define BOARD_HAS_VIBRATOR      0
#define BOARD_HAS_BUZZER        0

// ---------------------------------------------------------------------------
// I2C bus — unchanged from Rev 6
//   IMU       0x6A   LSM6DSV320X (same default address as LSM6DSO32X)
//   Fuel      0x36   MAX17048
//   Baro      0x77   BMP390 (DNP)
// ---------------------------------------------------------------------------
#define BOARD_I2C_PORT          0
#define BOARD_I2C_SDA_GPIO      8
#define BOARD_I2C_SCL_GPIO      9
#define BOARD_I2C_FREQ_HZ       400000

#define BOARD_IMU_I2C_ADDR      0x6A
#define BOARD_FUEL_I2C_ADDR     0x36
#define BOARD_BARO_I2C_ADDR     0x77

// ---------------------------------------------------------------------------
// IMU interrupts (LSM6DSV320X)
//   INT1 = MLC events, FSM events (routed via INT_CTRL registers)
//   INT2 = data-ready / FIFO threshold
//   Pin assignments preserved from Rev 6 so the IMU HAL wiring is rev-stable.
// ---------------------------------------------------------------------------
#define BOARD_IMU_INT1_GPIO     1
#define BOARD_IMU_INT2_GPIO     2

// ---------------------------------------------------------------------------
// Fuel gauge / charger — unchanged
// ---------------------------------------------------------------------------
#define BOARD_FUEL_ALRT_GPIO    3
#define BOARD_CHARGER_EN_N_GPIO 21  // active-low BQ25185 CE — PATRICK_TODO confirm

// ---------------------------------------------------------------------------
// SPI2 — LoRa only (CC1101 removed)
// ---------------------------------------------------------------------------
#define BOARD_SPI_HOST          2
#define BOARD_SPI_SCK_GPIO      11
#define BOARD_SPI_MOSI_GPIO     12
#define BOARD_SPI_MISO_GPIO     13

#define BOARD_LORA_CS_GPIO      10
#define BOARD_LORA_DIO0_GPIO    7
#define BOARD_LORA_RST_GPIO     15  // PATRICK_TODO

// GPIO 14 (formerly CC1101 CS) is now free. Assign in PCB layout if needed.

// ---------------------------------------------------------------------------
// I2S MEMS microphone (Tier 2 audio)
//   3-wire I2S: BCLK, WS (LRCLK), DIN. 16 kHz, 16-bit mono.
//   Pin selection: PATRICK_TODO. ESP32-S3 I2S is matrix-routable to most GPIOs.
// ---------------------------------------------------------------------------
#define BOARD_I2S_PORT          0
#define BOARD_I2S_BCLK_GPIO     4   // PATRICK_TODO
#define BOARD_I2S_WS_GPIO       5   // PATRICK_TODO  (LRCLK)
#define BOARD_I2S_DIN_GPIO      6   // PATRICK_TODO
#define BOARD_I2S_SAMPLE_RATE   16000
#define BOARD_I2S_BITS_PER_SAMP 16

// ---------------------------------------------------------------------------
// Always-on acoustic wake — DROPPED 2026-06-16 (decision log).
//   Motion (LSM6DSV320X MLC) is the sole wake source; no VAD chip. The earlier
//   candidate (Infineon IM73A135) was in any case an *analog* mic with no VAD —
//   not a wake-on-sound part. GPIO 39/40/41 are freed for PCB layout.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// GPS — unchanged from Rev 6
// ---------------------------------------------------------------------------
#define BOARD_GPS_UART_PORT     1
#define BOARD_GPS_UART_TX_GPIO  17
#define BOARD_GPS_UART_RX_GPIO  18
#define BOARD_GPS_UART_BAUD     9600

// ---------------------------------------------------------------------------
// USB — native S3
// ---------------------------------------------------------------------------
#define BOARD_USB_DM_GPIO       19
#define BOARD_USB_DP_GPIO       20

// ---------------------------------------------------------------------------
// Reserved — DO NOT USE: GPIO 35/36/37 (Octal-SPI PSRAM on -N16R8)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Power / battery
// ---------------------------------------------------------------------------
#define BOARD_BATTERY_MAH       500
#define BOARD_BATTERY_CHEMISTRY "LiPo 3.7V"
