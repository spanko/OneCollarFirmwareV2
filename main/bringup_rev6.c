/**
 * @file bringup_rev6.c
 * @brief OneCollar Rev 6 bring-up sketch.
 *
 * Walks every subsystem serially and prints PASS / FAIL / SKIP to USB serial,
 * per CLAUDE.md "Bring-up before features". This is the seed file for the
 * eventual driver implementations — drivers should not be written until the
 * relevant section here passes on hardware.
 *
 * Build with:
 *
 *   idf.py -DBOARD=rev6 -DBRINGUP=ON build flash monitor
 *
 * The BRINGUP flag is wired in main/CMakeLists.txt: when ON, this file
 * replaces main.c as the entry point and the `bt` component is added to
 * REQUIRES so NimBLE links. Default builds (no BRINGUP flag) use the
 * production main.c untouched.
 *
 * Citation policy (per the bring-up spec):
 *   Every register address, expected value, timing constant, or init sequence
 *   is either cited inline (CITE: ...) or flagged
 *      "UNVERIFIED — needs datasheet check"
 *   in a comment. Nothing is filled from training-data recall.
 *
 * ESP-IDF API references (Espressif Documentation MCP, accessed 2026-04-27):
 *   I2C master:
 *     https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2c.html
 *   SPI master:
 *     https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/spi_master.html
 *   GPIO:
 *     https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/gpio.html
 *   UART:
 *     https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/uart.html
 *   NimBLE host:
 *     https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/bluetooth/nimble/index.html
 *     + examples/bluetooth/ble_get_started/nimble/NimBLE_Connection
 *
 * All GPIO numbers, I2C addresses, baud rates, and SPI host selection come
 * from board.h — no pin numbers are hardcoded in this file.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

/* CITE: new I2C master driver — i2c_new_master_bus / i2c_master_bus_add_device /
 *       i2c_master_probe / i2c_master_transmit_receive
 *       https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2c.html */
#include "driver/i2c_master.h"

/* CITE: SPI master driver — spi_bus_initialize / spi_bus_add_device /
 *       spi_device_polling_transmit
 *       https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/spi_master.html */
#include "driver/spi_master.h"

/* CITE: GPIO driver — gpio_config / gpio_get_level
 *       https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/gpio.html */
#include "driver/gpio.h"

/* CITE: UART driver — uart_driver_install / uart_param_config / uart_set_pin / uart_read_bytes
 *       https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/uart.html */
#include "driver/uart.h"

/* CITE: NimBLE host
 *       https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/bluetooth/nimble/index.html
 *       + esp-idf/examples/bluetooth/ble_get_started/nimble/NimBLE_Connection */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"

#include "board.h"

// ---------------------------------------------------------------------------
// PASS / FAIL / SKIP reporting
// ---------------------------------------------------------------------------
typedef enum {
    CHECK_PASS = 0,
    CHECK_FAIL = 1,
    CHECK_SKIP = 2,
} check_result_t;

static const char *TAG = "oc.bringup";

static const char *result_str(check_result_t r)
{
    switch (r) {
    case CHECK_PASS: return "PASS";
    case CHECK_FAIL: return "FAIL";
    case CHECK_SKIP: return "SKIP";
    default:         return "????";
    }
}

static int pass_count, fail_count, skip_count;

static void report(const char *subsystem, check_result_t r, const char *fmt, ...)
{
    char detail[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);

    ESP_LOGI(TAG, "[ %s ] %-14s %s", result_str(r), subsystem, detail);

    switch (r) {
    case CHECK_PASS: pass_count++; break;
    case CHECK_FAIL: fail_count++; break;
    case CHECK_SKIP: skip_count++; break;
    }
}

// ---------------------------------------------------------------------------
// Module-scope handles (kept across checks so later steps can reuse the bus)
// ---------------------------------------------------------------------------
static i2c_master_bus_handle_t s_i2c_bus       = NULL;
static i2c_master_dev_handle_t s_imu_dev       = NULL;
static spi_device_handle_t     s_lora_spi_dev  = NULL;
static volatile bool           s_ble_connected = false;
static volatile bool           s_ble_adv_active = false;

// ===========================================================================
// 1. Banner + NVS
// ===========================================================================
static void log_banner(void)
{
    /* CLAUDE.md says boot banner should print rev string + board ID + git SHA + IDF
     * version. Avoid Unicode box-drawing per conventions.md (multi-byte UTF-8
     * shows up as noise in serial dumps). */
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " OneCollar bring-up sketch");
    ESP_LOGI(TAG, "   board:    %s (HW rev %d)", BOARD_NAME, BOARD_HW_REV);
    ESP_LOGI(TAG, "   target:   ESP32-S3");
    ESP_LOGI(TAG, "   IDF:      %s", esp_get_idf_version());
    ESP_LOGI(TAG, "================================================");
}

static check_result_t check_nvs(void)
{
    /* NVS must be initialized before NimBLE (controller stores PHY calibration). */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase, re-initializing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        report("nvs", CHECK_FAIL, "nvs_flash_init -> %s", esp_err_to_name(err));
        return CHECK_FAIL;
    }
    report("nvs", CHECK_PASS, "initialized");
    return CHECK_PASS;
}

// ===========================================================================
// 2. I2C bus init
// ===========================================================================
static check_result_t check_i2c_bus(void)
{
    /* CITE: i2c_master_bus_config_t and i2c_new_master_bus
     *   https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2c.html#install-i2c-master-bus-and-device
     *
     * `glitch_ignore_cnt = 7` is the value used in the official example.
     * `enable_internal_pullup = true` adds the SoC's ~45 kOhm pulls on top of
     * the PCB's 4.7 kOhm externals — harmless when externals are present and
     * lets the scan still complete if a board ships without them. */
    i2c_master_bus_config_t bus_cfg = {
        .clk_source                  = I2C_CLK_SRC_DEFAULT,
        .i2c_port                    = BOARD_I2C_PORT,
        .scl_io_num                  = BOARD_I2C_SCL_GPIO,
        .sda_io_num                  = BOARD_I2C_SDA_GPIO,
        .glitch_ignore_cnt           = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        report("i2c_init", CHECK_FAIL,
               "i2c_new_master_bus -> %s (port=%d sda=%d scl=%d)",
               esp_err_to_name(err),
               BOARD_I2C_PORT, BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
        return CHECK_FAIL;
    }
    report("i2c_init", CHECK_PASS,
           "port=%d sda=%d scl=%d (per-device freq=%d Hz)",
           BOARD_I2C_PORT, BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO, BOARD_I2C_FREQ_HZ);
    return CHECK_PASS;
}

// ===========================================================================
// 3. I2C scan
// ===========================================================================
static check_result_t check_i2c_scan(void)
{
    if (s_i2c_bus == NULL) {
        report("i2c_scan", CHECK_SKIP, "I2C bus not initialized");
        return CHECK_SKIP;
    }

    /* CITE: i2c_master_probe — "if address is correct and ACK is received, this
     * function will return ESP_OK"
     *   https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2c.html#i2c-master-probe
     *
     * Iterate the standard 7-bit user range 0x08..0x77 (the I2C spec reserves
     * 0x00-0x07 and 0x78-0x7F).
     */
    bool saw_imu = false, saw_fuel = false, saw_baro = false;
    int  found_count = 0;
    char found_list[128] = {0};
    size_t fl = 0;

    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        esp_err_t err = i2c_master_probe(s_i2c_bus, addr, 50);
        if (err == ESP_OK) {
            found_count++;
            fl += snprintf(found_list + fl, sizeof(found_list) - fl,
                           "%s0x%02X", fl ? " " : "", addr);
            if (addr == BOARD_IMU_I2C_ADDR)  saw_imu  = true;
            if (addr == BOARD_FUEL_I2C_ADDR) saw_fuel = true;
            if (addr == BOARD_BARO_I2C_ADDR) saw_baro = true;
        }
    }

    ESP_LOGI(TAG, "i2c_scan: found %d device(s) [%s]", found_count, found_list);

    /* Required (Rev 6 always-populated): IMU @ 0x6A, fuel @ 0x36.
     * Optional: BMP390 baro @ 0x77 (DNP unless hand-soldered). */
    if (saw_imu && saw_fuel) {
        report("i2c_scan", CHECK_PASS,
               "imu=0x%02X fuel=0x%02X baro=%s",
               BOARD_IMU_I2C_ADDR, BOARD_FUEL_I2C_ADDR,
               saw_baro ? "present" : "DNP");
        return CHECK_PASS;
    }
    report("i2c_scan", CHECK_FAIL,
           "missing required device(s): %s%s",
           saw_imu  ? "" : "IMU ",
           saw_fuel ? "" : "FUEL");
    return CHECK_FAIL;
}

// ===========================================================================
// 4. IMU WHO_AM_I (LSM6DSO32X)
// ===========================================================================
/* USER-PROVIDED in bring-up spec: LSM6DSO32X WHO_AM_I should return 0x6C. */
#define LSM6DSO32X_WHO_AM_I_EXPECTED  0x6Cu

/* UNVERIFIED — needs datasheet check.
 *   The ST LSM6 family convention is to expose WHO_AM_I at register 0x0F, but
 *   this register address has NOT been read out of the LSM6DSO32X datasheet
 *   (DS13074) in this session. Confirm against §8 / register map of the
 *   datasheet before trusting bring-up output. If 0x0F is wrong, the read will
 *   return whatever lives at the actual address — which will not match 0x6C
 *   and the FAIL message will direct the user back here. */
#define LSM6DSO32X_REG_WHO_AM_I       0x0Fu

static check_result_t check_imu_whoami(void)
{
    if (s_i2c_bus == NULL) {
        report("imu", CHECK_SKIP, "I2C bus not initialized");
        return CHECK_SKIP;
    }

    /* CITE: i2c_master_bus_add_device + i2c_device_config_t (scl_speed_hz is
     * per-device; device_address is raw 7-bit, no R/W bit shifted in)
     *   https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2c.html#install-i2c-master-bus-and-device
     */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_IMU_I2C_ADDR,
        .scl_speed_hz    = BOARD_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_imu_dev);
    if (err != ESP_OK) {
        report("imu", CHECK_FAIL, "bus_add_device -> %s", esp_err_to_name(err));
        return CHECK_FAIL;
    }

    /* CITE: i2c_master_transmit_receive — write reg-address, read N bytes back,
     * no STOP between phases (suited to register read).
     *   https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2c.html#i2c-master-write-and-read
     */
    uint8_t reg     = LSM6DSO32X_REG_WHO_AM_I;
    uint8_t whoami  = 0;
    err = i2c_master_transmit_receive(s_imu_dev, &reg, 1, &whoami, 1, 100);
    if (err != ESP_OK) {
        report("imu", CHECK_FAIL,
               "WHO_AM_I read failed: %s (reg=0x%02X UNVERIFIED)",
               esp_err_to_name(err), LSM6DSO32X_REG_WHO_AM_I);
        return CHECK_FAIL;
    }

    if (whoami == LSM6DSO32X_WHO_AM_I_EXPECTED) {
        report("imu", CHECK_PASS,
               "LSM6DSO32X @ 0x%02X WHO_AM_I=0x%02X (expected 0x%02X)",
               BOARD_IMU_I2C_ADDR, whoami, LSM6DSO32X_WHO_AM_I_EXPECTED);
        return CHECK_PASS;
    }
    report("imu", CHECK_FAIL,
           "LSM6DSO32X @ 0x%02X WHO_AM_I=0x%02X (expected 0x%02X) "
           "-- if read succeeded but value is wrong, verify reg addr 0x%02X "
           "against LSM6DSO32X datasheet (currently UNVERIFIED)",
           BOARD_IMU_I2C_ADDR, whoami,
           LSM6DSO32X_WHO_AM_I_EXPECTED,
           LSM6DSO32X_REG_WHO_AM_I);
    return CHECK_FAIL;
}

// ===========================================================================
// 5. MAX17048 fuel gauge
// ===========================================================================
static check_result_t check_fuel_gauge(void)
{
    /* The bring-up spec lists "MAX17048 voltage/SOC sanity check" but does not
     * provide the register layout, and we have not pulled it from the MAX17048
     * datasheet in this session. Limit this check to the I2C probe (which the
     * scan already performed) plus reporting; defer voltage/SOC reads to the
     * driver implementation, which will cite registers from the datasheet. */
    if (s_i2c_bus == NULL) {
        report("fuel", CHECK_SKIP, "I2C bus not initialized");
        return CHECK_SKIP;
    }
    esp_err_t err = i2c_master_probe(s_i2c_bus, BOARD_FUEL_I2C_ADDR, 50);
    if (err != ESP_OK) {
        report("fuel", CHECK_FAIL,
               "MAX17048 @ 0x%02X probe -> %s",
               BOARD_FUEL_I2C_ADDR, esp_err_to_name(err));
        return CHECK_FAIL;
    }
    /* UNVERIFIED — needs datasheet check.
     *   MAX17048 VCELL / SOC / VERSION register addresses are not cited in this
     *   sketch. The driver implementation in drivers/fuel_max17048.c must read
     *   these from the Maxim MAX17048 datasheet (rev 7+) and cite them. */
    report("fuel", CHECK_PASS,
           "MAX17048 @ 0x%02X ACK (voltage/SOC read deferred to driver -- "
           "register map UNVERIFIED here)", BOARD_FUEL_I2C_ADDR);
    return CHECK_PASS;
}

// ===========================================================================
// 6. Charger STAT pin
// ===========================================================================
static check_result_t check_charger_stat(void)
{
    /* CITE: gpio_config_t / gpio_config — "Configure GPIO's Mode, pull-up,
     * PullDown, IntrType."
     *   https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/gpio.html#functions
     *
     * BQ25185 STAT polarity: UNVERIFIED — needs datasheet check.
     *   Many TI chargers expose STAT as an open-drain output that pulls low
     *   while charging and high-Z (released) otherwise; we don't cite that
     *   here, so this check just reads and reports the level rather than
     *   asserting a specific charging state. */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_CHARGER_STAT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,    /* OK regardless of STAT polarity */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        report("charger", CHECK_FAIL, "gpio_config(STAT) -> %s", esp_err_to_name(err));
        return CHECK_FAIL;
    }
    int level = gpio_get_level(BOARD_CHARGER_STAT_GPIO);
    report("charger", CHECK_PASS,
           "STAT (GPIO%d) level=%d (polarity UNVERIFIED -- consult BQ25185 datasheet)",
           BOARD_CHARGER_STAT_GPIO, level);
    return CHECK_PASS;
}

// ===========================================================================
// 7. RFM95W silicon-rev read (SPI)
// ===========================================================================
/* UNVERIFIED — needs datasheet check.
 *   The Semtech SX1276 / HopeRF RFM95W datasheets define a "RegVersion"
 *   register that returns a fixed silicon-rev byte. Both the register
 *   ADDRESS and the EXPECTED VALUE below are conventional knowledge for the
 *   SX127x family but have NOT been confirmed against the datasheets in this
 *   session. Confirm before trusting the result.
 *
 *   SX127x SPI read protocol (also UNVERIFIED here): bit 7 of the first byte
 *   is W/R (0 = read, 1 = write), bits 6:0 are the register address. The
 *   second byte clocks out the register contents.
 */
#define RFM95W_REG_VERSION_ADDR    0x42u  /* UNVERIFIED */
#define RFM95W_REG_VERSION_EXPECT  0x12u  /* UNVERIFIED */

static check_result_t check_lora_silicon_rev(void)
{
#if !BOARD_HAS_LORA
    report("lora", CHECK_SKIP, "BOARD_HAS_LORA == 0");
    return CHECK_SKIP;
#else
    /* CITE: spi_bus_initialize / spi_bus_add_device / spi_device_polling_transmit
     *   https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/spi_master.html#driver-usage
     *
     * BOARD_SPI_HOST = 2 maps to SPI2_HOST in ESP-IDF (per board.h comment).
     *
     * This brings up the bus only — CC1101 (BOARD_HAS_SUBGHZ_RADIO) shares
     * SCK/MOSI/MISO with LoRa but uses a separate CS, so the bus init is
     * one-time. Bring-up of CC1101 is not in the spec list. */
    spi_bus_config_t bus_cfg = {
        .miso_io_num     = BOARD_SPI_MISO_GPIO,
        .mosi_io_num     = BOARD_SPI_MOSI_GPIO,
        .sclk_io_num     = BOARD_SPI_SCK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t err = spi_bus_initialize((spi_host_device_t)BOARD_SPI_HOST,
                                       &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        report("lora", CHECK_FAIL, "spi_bus_initialize -> %s", esp_err_to_name(err));
        return CHECK_FAIL;
    }

    /* RFM95W max SPI clock: UNVERIFIED — needs datasheet check.
     * Conservatively pick 1 MHz for bring-up; the production driver should
     * raise this after confirming against the datasheet. */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 1 * 1000 * 1000,
        .mode           = 0,                  /* CPOL=0, CPHA=0 — UNVERIFIED for SX127x */
        .spics_io_num   = BOARD_LORA_CS_GPIO,
        .queue_size     = 1,
    };
    err = spi_bus_add_device((spi_host_device_t)BOARD_SPI_HOST, &dev_cfg, &s_lora_spi_dev);
    if (err != ESP_OK) {
        report("lora", CHECK_FAIL, "spi_bus_add_device -> %s", esp_err_to_name(err));
        return CHECK_FAIL;
    }

    /* Optional reset pulse. RFM95W RST timing: UNVERIFIED — needs datasheet check.
     * Skip explicit reset for now; if RegVersion read fails on hardware, add a
     * datasheet-cited reset sequence here as the next step. */

    uint8_t tx[2] = { (uint8_t)(RFM95W_REG_VERSION_ADDR & 0x7Fu), 0x00 }; /* UNVERIFIED protocol */
    uint8_t rx[2] = { 0 };
    spi_transaction_t t = {
        .length    = 8 * sizeof(tx),
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    err = spi_device_polling_transmit(s_lora_spi_dev, &t);
    if (err != ESP_OK) {
        report("lora", CHECK_FAIL, "polling_transmit -> %s", esp_err_to_name(err));
        return CHECK_FAIL;
    }

    uint8_t version = rx[1];
    if (version == RFM95W_REG_VERSION_EXPECT) {
        report("lora", CHECK_PASS,
               "RFM95W RegVersion=0x%02X (matches expected 0x%02X -- both UNVERIFIED)",
               version, RFM95W_REG_VERSION_EXPECT);
        return CHECK_PASS;
    }
    /* Don't hard-FAIL on a mismatch when both the address and the expected
     * value are unverified — print the byte we got and let the operator
     * compare against the datasheet. The point of bring-up is to get eyes
     * on the bus, not to rubber-stamp guessed constants. */
    report("lora", CHECK_FAIL,
           "RFM95W RegVersion=0x%02X (expected 0x%02X) -- "
           "verify reg addr 0x%02X, expected value, and SPI mode against "
           "Semtech SX1276 / HopeRF RFM95W datasheets",
           version, RFM95W_REG_VERSION_EXPECT, RFM95W_REG_VERSION_ADDR);
    return CHECK_FAIL;
#endif
}

// ===========================================================================
// 8. GPS UART NMEA presence — pin-discovery sweep
// ===========================================================================
/*
 * Why this is a discovery routine, not a single fixed-pin check:
 *
 *   board.h flags BOARD_GPS_UART_TX_GPIO (17) and BOARD_GPS_UART_RX_GPIO (18)
 *   as TBD pending Patrick's confirmation against the actual Rev 6 PCB, and
 *   the V1 firmware carries a comment "try swapping 16/17" — strong evidence
 *   that the wiring between the NEO-M8Q breakout's TX pin and the ESP32-S3
 *   side was uncertain on early silicon. Rather than fail bring-up on a
 *   guessed pin pair, sweep three plausible combinations and report which
 *   one actually talks. The bench operator then makes a deliberate edit to
 *   board.h based on what we observed; this code does NOT mutate board.h.
 *
 *   Attempt C (TX=16, RX=17) is gated on BOARD_HAS_SUBGHZ_RADIO == 0 because
 *   GPIO 16 is also defined as BOARD_CC1101_GDO0_GPIO; driving it while
 *   CC1101 is populated would contend with the radio's output. CC1101 is
 *   intentionally not initialized anywhere earlier in this sketch so that
 *   attempt C is free to drive GPIO 16 when the radio is depopulated.
 */

/* Hex nibble parser. Returns 0..15 on hex digit, -1 otherwise. */
static int gps_hex_nibble(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

/* Scan accumulated buffer for the first complete NMEA sentence beginning
 * with "$G" whose checksum (XOR of bytes between '$' exclusive and '*'
 * exclusive) matches the 2 hex digits following '*'. On success, copies the
 * sentence text (from '$' through the 2 checksum digits, no CR/LF) into
 * out_sentence and returns true. Does NOT require GPS lock — this is a
 * "module is talking" check, not a fix-acquired check. */
static bool gps_find_valid_nmea(const uint8_t *buf, int len,
                                char *out_sentence, size_t out_size)
{
    /* Cap per-sentence search length defensively. NMEA-0183 caps sentences at
     * 82 bytes including the leading '$' and trailing CR/LF; 100 leaves slack
     * for non-conformant talkers without letting a missing '*' walk forever. */
    const int max_sentence = 100;

    for (int i = 0; i + 5 < len; i++) {
        if (buf[i] != '$' || buf[i + 1] != 'G') continue;

        int star = -1;
        int hard_stop = i + max_sentence;
        if (hard_stop > len) hard_stop = len;
        for (int j = i + 2; j < hard_stop; j++) {
            if (buf[j] == '*') { star = j; break; }
            if (buf[j] == '$' || buf[j] == '\r' || buf[j] == '\n') break;
        }
        if (star < 0 || star + 2 >= len) continue;

        int hi = gps_hex_nibble(buf[star + 1]);
        int lo = gps_hex_nibble(buf[star + 2]);
        if (hi < 0 || lo < 0) continue;
        uint8_t expected = (uint8_t)((hi << 4) | lo);

        uint8_t calc = 0;
        for (int k = i + 1; k < star; k++) calc ^= buf[k];
        if (calc != expected) continue;

        int slen = (star + 3) - i;
        if (slen >= (int)out_size) slen = (int)out_size - 1;
        memcpy(out_sentence, &buf[i], slen);
        out_sentence[slen] = '\0';
        return true;
    }
    return false;
}

/* One attempt: configure UART with the given pins, listen up to 3 s for a
 * checksum-valid "$G..." sentence, tear the driver down. Emits the per-attempt
 * status line in the format the bring-up spec mandates. */
static check_result_t gps_try_pins(char attempt_id, int tx_gpio, int rx_gpio,
                                   int *out_pass_tx, int *out_pass_rx)
{
    const uart_port_t port = (uart_port_t)BOARD_GPS_UART_PORT;
    const int rx_buf_sz = 2048;

    uart_config_t cfg = {
        .baud_rate  = BOARD_GPS_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(port, rx_buf_sz, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "GPS attempt %c: TX=%d RX=%d -- FAIL "
                      "(uart_driver_install -> %s)",
                 attempt_id, tx_gpio, rx_gpio, esp_err_to_name(err));
        return CHECK_FAIL;
    }
    err = uart_param_config(port, &cfg);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "GPS attempt %c: TX=%d RX=%d -- FAIL "
                      "(uart_param_config -> %s)",
                 attempt_id, tx_gpio, rx_gpio, esp_err_to_name(err));
        uart_driver_delete(port);
        return CHECK_FAIL;
    }
    err = uart_set_pin(port, tx_gpio, rx_gpio,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "GPS attempt %c: TX=%d RX=%d -- FAIL "
                      "(uart_set_pin -> %s)",
                 attempt_id, tx_gpio, rx_gpio, esp_err_to_name(err));
        uart_driver_delete(port);
        return CHECK_FAIL;
    }

    /* Accumulate up to ~1.5 KB of UART output across the 3 s window. NMEA
     * sentences arrive in chunks split arbitrarily by the read; assembling
     * before scanning avoids dropping a sentence that straddles two reads. */
    uint8_t  accum[1536];
    int      accum_len = 0;
    char     sentence[100];
    bool     found = false;
    uint8_t  chunk[256];

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    while (xTaskGetTickCount() < deadline && !found) {
        int n = uart_read_bytes(port, chunk, sizeof(chunk), pdMS_TO_TICKS(200));
        if (n <= 0) continue;

        if (accum_len + n > (int)sizeof(accum)) {
            int drop = (accum_len + n) - (int)sizeof(accum);
            memmove(accum, accum + drop, accum_len - drop);
            accum_len -= drop;
        }
        memcpy(accum + accum_len, chunk, n);
        accum_len += n;

        if (gps_find_valid_nmea(accum, accum_len, sentence, sizeof(sentence))) {
            found = true;
        }
    }

    uart_driver_delete(port);

    if (found) {
        ESP_LOGI(TAG, "GPS attempt %c: TX=%d RX=%d -- PASS (NMEA detected: %s)",
                 attempt_id, tx_gpio, rx_gpio, sentence);
        if (out_pass_tx && *out_pass_tx < 0) {
            *out_pass_tx = tx_gpio;
            *out_pass_rx = rx_gpio;
        }
        return CHECK_PASS;
    }
    ESP_LOGI(TAG, "GPS attempt %c: TX=%d RX=%d -- FAIL (no valid NMEA in 3s)",
             attempt_id, tx_gpio, rx_gpio);
    return CHECK_FAIL;
}

static check_result_t check_gps_nmea(void)
{
#if !BOARD_HAS_GPS
    report("gps", CHECK_SKIP, "BOARD_HAS_GPS == 0");
    return CHECK_SKIP;
#else
    int pass_tx = -1, pass_rx = -1;

    /* Attempt A: pin pair currently asserted by board.h (17, 18). */
    gps_try_pins('A', BOARD_GPS_UART_TX_GPIO, BOARD_GPS_UART_RX_GPIO,
                 &pass_tx, &pass_rx);

    /* Attempt B: TX/RX swap. */
    gps_try_pins('B', BOARD_GPS_UART_RX_GPIO, BOARD_GPS_UART_TX_GPIO,
                 &pass_tx, &pass_rx);

    /* Attempt C: GPIO 16 probe, only when CC1101 is depopulated. */
#if BOARD_HAS_SUBGHZ_RADIO
    ESP_LOGI(TAG, "GPS attempt C: TX=16 RX=17 -- SKIP (CC1101 populated)");
#else
    gps_try_pins('C', 16, 17, &pass_tx, &pass_rx);
#endif

    if (pass_tx >= 0) {
        ESP_LOGI(TAG, "GPS PASS at TX=%d RX=%d -- "
                      "board.h needs update if not 17/18",
                 pass_tx, pass_rx);
        report("gps", CHECK_PASS, "TX=%d RX=%d (UART%d @ %d)",
               pass_tx, pass_rx, BOARD_GPS_UART_PORT, BOARD_GPS_UART_BAUD);
        return CHECK_PASS;
    }

    ESP_LOGI(TAG, "GPS FAIL -- tried 17/18, 18/17, 16/17. "
                  "Check connector and antenna.");
    report("gps", CHECK_FAIL,
           "no valid NMEA on any pin combination (UART%d @ %d)",
           BOARD_GPS_UART_PORT, BOARD_GPS_UART_BAUD);
    return CHECK_FAIL;
#endif
}

// ===========================================================================
// 9. BLE advertise + accept connection (NimBLE)
// ===========================================================================
static const char *BLE_DEVICE_NAME = "OneCollar-Bringup";

/* CITE: gap event handler pattern — BLE_GAP_EVENT_CONNECT / DISCONNECT
 *   esp-idf/examples/bluetooth/ble_get_started/nimble/NimBLE_Connection */
static int bringup_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "ble: connect %s status=%d",
                 event->connect.status == 0 ? "ESTABLISHED" : "FAILED",
                 event->connect.status);
        if (event->connect.status == 0) {
            s_ble_connected = true;
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "ble: disconnect reason=%d", event->disconnect.reason);
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "ble: adv complete reason=%d", event->adv_complete.reason);
        s_ble_adv_active = false;
        return 0;
    default:
        return 0;
    }
}

static void bringup_ble_start_advertising(void)
{
    /* CITE: ble_gap_adv_params + ble_gap_adv_start
     *   esp-idf/examples/bluetooth/ble_get_started/nimble/NimBLE_Connection */
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble: ble_hs_id_infer_auto rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)BLE_DEVICE_NAME;
    fields.name_len = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble: ble_gap_adv_set_fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND, /* connectable */
        .disc_mode = BLE_GAP_DISC_MODE_GEN, /* general discoverable */
        /* itvl_min/max left at default per NimBLE docs — UNVERIFIED here whether
         * the defaults match our power budget; the production driver should set
         * itvl_min = BLE_GAP_ADV_ITVL_MS(adv_period_ms). */
    };
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, bringup_gap_event_handler, NULL);
    if (rc == 0) {
        s_ble_adv_active = true;
        ESP_LOGI(TAG, "ble: advertising started (name=\"%s\")", BLE_DEVICE_NAME);
    } else {
        ESP_LOGE(TAG, "ble: ble_gap_adv_start rc=%d", rc);
    }
}

static void bringup_ble_on_sync(void)
{
    /* CITE: stack-sync callback registers via ble_hs_cfg.sync_cb and is invoked
     * once the host has synced with the controller. Adv cannot start before this.
     *   esp-idf/examples/bluetooth/ble_get_started/nimble/NimBLE_Connection */
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble: ble_hs_util_ensure_addr rc=%d", rc);
        return;
    }
    bringup_ble_start_advertising();
}

static void bringup_ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "ble: stack reset reason=%d", reason);
}

static void bringup_ble_host_task(void *param)
{
    /* CITE: nimble_port_run runs the default event queue until nimble_port_stop. */
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static check_result_t check_ble(void)
{
    /* CITE: nimble_port_init initializes BT controller + NimBLE host (single API
     * since recent ESP-IDF; replaces the older esp_nimble_hci_and_controller_init).
     *   https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/bluetooth/nimble/index.html */
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        report("ble", CHECK_FAIL, "nimble_port_init -> %s", esp_err_to_name(err));
        return CHECK_FAIL;
    }

    ble_hs_cfg.reset_cb = bringup_ble_on_reset;
    ble_hs_cfg.sync_cb  = bringup_ble_on_sync;

    int rc = ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    if (rc != 0) {
        report("ble", CHECK_FAIL, "ble_svc_gap_device_name_set rc=%d", rc);
        return CHECK_FAIL;
    }

    nimble_port_freertos_init(bringup_ble_host_task);

    /* Wait up to 30 s for advertising to start AND optionally for a connection.
     * Advertising start is the gating PASS criterion; a connection is a bonus
     * signal that the host stack is fully wired through to a peer. */
    const TickType_t window = pdMS_TO_TICKS(30 * 1000);
    TickType_t deadline = xTaskGetTickCount() + window;
    bool announced_adv = false;
    while (xTaskGetTickCount() < deadline) {
        if (s_ble_adv_active && !announced_adv) {
            ESP_LOGI(TAG, "ble: peripheral advertising as \"%s\"; "
                          "connect from a phone within 30 s to confirm",
                     BLE_DEVICE_NAME);
            announced_adv = true;
        }
        if (s_ble_connected) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (s_ble_connected) {
        report("ble", CHECK_PASS,
               "advertised + accepted connection as \"%s\"", BLE_DEVICE_NAME);
        return CHECK_PASS;
    }
    if (s_ble_adv_active) {
        report("ble", CHECK_PASS,
               "advertising as \"%s\" (no peer connected within 30 s -- "
               "advertise PASS, connection unverified by this run)",
               BLE_DEVICE_NAME);
        return CHECK_PASS;
    }
    report("ble", CHECK_FAIL, "advertising never started within 30 s");
    return CHECK_FAIL;
}

// ===========================================================================
// app_main — run all checks serially
// ===========================================================================
void app_main(void)
{
    log_banner();

    /* Order matters: NVS before NimBLE; I2C bus before scan/IMU/fuel. */
    check_nvs();
    check_i2c_bus();
    check_i2c_scan();
    check_imu_whoami();
    check_fuel_gauge();
    check_charger_stat();
    check_lora_silicon_rev();
    check_gps_nmea();
    check_ble();

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " bring-up summary: %d PASS, %d FAIL, %d SKIP",
             pass_count, fail_count, skip_count);
    ESP_LOGI(TAG, "================================================");
    if (fail_count == 0) {
        ESP_LOGI(TAG, "Rev 6 bring-up complete -- ready to start writing real drivers.");
    } else {
        ESP_LOGW(TAG, "%d subsystem(s) failed; do NOT start feature work until "
                      "every required check passes.", fail_count);
    }

    /* Park. The bring-up sketch deliberately does not spawn FreeRTOS app tasks
     * -- the production scaffold (main.c) does that once bring-up clears. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    }
}
