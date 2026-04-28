/**
 * @file board.h
 * @brief OneCollar Rev 6 — pin map and capability flags.
 *
 * Hardware: ESP32-S3-WROOM-1-N16R8 + STMicro LSM6DSO32X (IMU) +
 *           MAX17048 (fuel gauge) + RFM95W (LoRa) + NEO-M8Q (GPS, external) +
 *           BMP390 (barometer, DNP).
 *
 * Drivers consume capability flags (BOARD_HAS_*) to select code paths.
 * No GPIO numbers should appear outside this file.
 *
 * NOTE: Pin assignments mirror the OneCollarFirmwareV2 scaffold's original
 * board.h (Rev 5-S3 era). Confirm with Patrick against current Rev 6 PCB
 * before committing to first-silicon firmware bring-up.
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
// I2C bus — shared at 400 kHz fast mode
//   IMU       0x6A   LSM6DSO32X
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
// IMU interrupts (LSM6DSO32X)
//   INT1 = MLC / wake-on-activity
//   INT2 = data-ready @ 104 Hz
// ---------------------------------------------------------------------------
#define BOARD_IMU_INT1_GPIO     1
#define BOARD_IMU_INT2_GPIO     2

// ---------------------------------------------------------------------------
// Fuel gauge alert / charger status
// ---------------------------------------------------------------------------
#define BOARD_FUEL_ALRT_GPIO    3   // active low
#define BOARD_CHARGER_STAT_GPIO 21

// ---------------------------------------------------------------------------
// SPI2 — shared between LoRa (RFM95W) and CC1101
// ---------------------------------------------------------------------------
#define BOARD_SPI_HOST          2   // SPI2_HOST in ESP-IDF
#define BOARD_SPI_SCK_GPIO      11
#define BOARD_SPI_MOSI_GPIO     12
#define BOARD_SPI_MISO_GPIO     13

#define BOARD_LORA_CS_GPIO      10
#define BOARD_LORA_DIO0_GPIO    7
#define BOARD_LORA_RST_GPIO     15  // TBD — confirm with Patrick

#define BOARD_CC1101_CS_GPIO    14
#define BOARD_CC1101_GDO0_GPIO  16  // TBD — confirm with Patrick

// ---------------------------------------------------------------------------
// GPS (NEO-M8Q via UART1, external 6-pin JST-SH)
// ---------------------------------------------------------------------------
#define BOARD_GPS_UART_PORT     1
#define BOARD_GPS_UART_TX_GPIO  17  // TBD — V1 firmware notes "try swapping" 16/17
#define BOARD_GPS_UART_RX_GPIO  18  // TBD — confirm against Rev 6 PCB
#define BOARD_GPS_UART_BAUD     9600

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
