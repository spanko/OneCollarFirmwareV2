/**
 * @file board.h
 * @brief OneCollar Rev 6 — pin map and capability flags.
 *
 * Hardware: ESP32-S3-WROOM-1-N16R8 + STMicro LSM6DSO32X (IMU) +
 *           MAX17048 (fuel gauge) + BQ25185 (charger) + RFM95W (LoRa) +
 *           NEO-M8Q (GPS, external 6-pin JST-SH breakout) + CC1101 (sub-GHz) +
 *           BMP390 (barometer, DNP).
 *
 * Drivers consume capability flags (BOARD_HAS_*) to select code paths.
 * No GPIO numbers should appear outside this file.
 *
 * Pin map is now grounded in Patrick's Rev 6 schematic (matches the
 * OneCollar_HW_Bringup.ino sketch that successfully exercised first silicon
 * on the bench). Pre-Rev-6 scaffold values from the Rev 5-S3 era have been
 * replaced.
 */

#pragma once

// ---------------------------------------------------------------------------
// Board identity
// ---------------------------------------------------------------------------
#define BOARD_NAME              "OneCollar Rev 6"
#define BOARD_HW_REV            6
#define BOARD_PCB_DATE          "TBD — Patrick to confirm"

// ---------------------------------------------------------------------------
// Capability flags — drivers branch on these. Set 1 if populated, 0 if absent.
// ---------------------------------------------------------------------------
#define BOARD_HAS_IMU           1
#define BOARD_HAS_IMU_LOWG      1   // baseline 6-DOF accel+gyro
#define BOARD_HAS_IMU_HIGHG     0   // dedicated high-g channel — Rev 7+
#define BOARD_HAS_IMU_SFLP      0   // chip-native gravity/quaternion — Rev 7+
#define BOARD_HAS_IMU_MLC       1   // 8 trees, 256 nodes (DSO32X)
#define BOARD_HAS_FUEL_GAUGE    1
#define BOARD_HAS_BAROMETER     0   // BMP390 footprint present, DNP
#define BOARD_HAS_AUDIO         0   // I2S MEMS mic — Rev 7+
#define BOARD_HAS_VAD           0   // Infineon PDM+VAD — Rev 7 evaluation
#define BOARD_HAS_GPS           1   // NEO-M8Q via 6-pin JST-SH external breakout
#define BOARD_HAS_LORA          1   // RFM95W
#define BOARD_HAS_SUBGHZ_RADIO  1   // CC1101 — proposed for removal in Rev 7
#define BOARD_HAS_LED_RGB       0
#define BOARD_HAS_VIBRATOR      0
#define BOARD_HAS_BUZZER        0

// ---------------------------------------------------------------------------
// I2C bus — shared
//   IMU       0x6A   LSM6DSO32X
//   Fuel      0x36   MAX17048
//   Baro      0x77   BMP390 (DNP, hand-solder only)
//
// Bring-up firmware starts at 100 kHz for safety on first-silicon boards;
// production drivers use BOARD_I2C_FREQ_HZ (400 kHz fast mode).
// ---------------------------------------------------------------------------
#define BOARD_I2C_PORT          0
#define BOARD_I2C_SDA_GPIO      2
#define BOARD_I2C_SCL_GPIO      1
#define BOARD_I2C_FREQ_HZ       400000

#define BOARD_IMU_I2C_ADDR      0x6A
#define BOARD_FUEL_I2C_ADDR     0x36
#define BOARD_BARO_I2C_ADDR     0x77

// ---------------------------------------------------------------------------
// IMU interrupts (LSM6DSO32X)
//   INT1 = MLC / wake-on-activity
//   INT2 = data-ready @ 104 Hz
// ---------------------------------------------------------------------------
#define BOARD_IMU_INT1_GPIO     47
#define BOARD_IMU_INT2_GPIO     46

// ---------------------------------------------------------------------------
// Fuel gauge alert / charger enable
//   MAX17048 ALRT is open-drain, active-low.
//   BQ25185 CE (CHGR-EN-N) is an active-low enable input driven by the MCU.
// ---------------------------------------------------------------------------
#define BOARD_FUEL_ALRT_GPIO    17  // active low input
#define BOARD_CHARGER_EN_N_GPIO 21  // active low output to BQ25185 CE

// ---------------------------------------------------------------------------
// SPI2 — shared between LoRa (RFM95W) and CC1101
// ---------------------------------------------------------------------------
#define BOARD_SPI_HOST          2   // SPI2_HOST in ESP-IDF
#define BOARD_SPI_SCK_GPIO      40
#define BOARD_SPI_MOSI_GPIO     39
#define BOARD_SPI_MISO_GPIO     38

#define BOARD_LORA_CS_GPIO      41
#define BOARD_LORA_RST_GPIO     42  // RFM95 RESET, active low
#define BOARD_LORA_DIO0_GPIO    3
#define BOARD_LORA_DIO1_GPIO    18

#define BOARD_CC1101_CS_GPIO    45
#define BOARD_CC1101_GDO0_GPIO  11  // GDO0 / ATEST
#define BOARD_CC1101_GDO2_GPIO  12

// ---------------------------------------------------------------------------
// GPS (NEO-M8Q via UART1, external 6-pin JST-SH)
//   *_TX_GPIO is the MCU-driven line (-> GPS RXD).
//   *_RX_GPIO is the MCU-listening line (<- GPS TXD).
// ---------------------------------------------------------------------------
#define BOARD_GPS_UART_PORT         1
#define BOARD_GPS_UART_TX_GPIO      9   // MCU TX -> GPS RXD
#define BOARD_GPS_UART_RX_GPIO      10  // GPS TXD -> MCU RX
#define BOARD_GPS_UART_BAUD         9600
#define BOARD_GPS_PPS_GPIO          7   // TIMEPULSE input
#define BOARD_GPS_RESET_N_GPIO      8   // active low reset output
#define BOARD_GPS_SAFEBOOT_N_GPIO   6   // active low SAFEBOOT_N output

// ---------------------------------------------------------------------------
// USB (native to ESP32-S3 — exposes /dev/ttyACM0 on Linux, COMx on Windows)
// ---------------------------------------------------------------------------
#define BOARD_USB_DM_GPIO       19
#define BOARD_USB_DP_GPIO       20

// ---------------------------------------------------------------------------
// Reserved — DO NOT USE
//   GPIO 35/36/37 are bonded to the Octal-SPI PSRAM on -N16R8 modules.
//   PSRAM is not used by application code (8 MB available but unsurfaced).
// ---------------------------------------------------------------------------
// (no defines — listed here for awareness)

// ---------------------------------------------------------------------------
// Power / battery
// ---------------------------------------------------------------------------
#define BOARD_BATTERY_MAH       500
#define BOARD_BATTERY_CHEMISTRY "LiPo 3.7V"
