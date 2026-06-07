/**
 * @file bringup_rev6.c
 * @brief OneCollar Rev 6 bring-up: automatic walk on boot, then interactive REPL.
 *
 * Boot sequence:
 *   1. Banner + NVS init
 *   2. Walk every subsystem and print PASS / FAIL / SKIP
 *   3. Drop into a serial REPL over the S3 native USB (USB-Serial-JTAG,
 *      /dev/ttyACM0 on Linux). Operator drives further investigation.
 *
 * Build:
 *   idf.py -DBOARD=rev6 -DBRINGUP=ON build flash monitor
 *
 * History: ported from Patrick's `OneCollar_HW_Bringup.ino` (Arduino) into
 * native ESP-IDF. The .ino has been bench-validated against Rev 6 silicon —
 * its pin map is now reflected in boards/rev6/board.h, and its register
 * sequences (LSM6DSO32X init, MAX17048 V/SOC, RFM95 RegVersion, CC1101
 * status reads) are cited in this file as ground truth.
 *
 * Citation policy:
 *   Register addresses, expected values, and timings either cite the .ino
 *   (bench-validated source of truth) or are marked
 *       UNVERIFIED — needs datasheet check
 *
 * GPIO numbers come from board.h only.
 *
 * ESP-IDF API references (Espressif Documentation, v6.0):
 *   I2C master:   esp32s3/api-reference/peripherals/i2c.html
 *   SPI master:   esp32s3/api-reference/peripherals/spi_master.html
 *   GPIO:         esp32s3/api-reference/peripherals/gpio.html
 *   UART:         esp32s3/api-reference/peripherals/uart.html
 *   USB-Serial-JTAG console: esp32s3/api-guides/usb-serial-jtag-console.html
 *   NimBLE host:  esp32s3/api-reference/bluetooth/nimble/index.html
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_gap.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "board.h"

static const char *TAG = "oc.bringup";

// ===========================================================================
// PASS / FAIL / SKIP infrastructure
// ===========================================================================
typedef enum {
    CHECK_PASS = 0,
    CHECK_FAIL = 1,
    CHECK_SKIP = 2,
} check_result_t;

static int s_pass_count, s_fail_count, s_skip_count;

static const char *result_str(check_result_t r)
{
    switch (r) {
    case CHECK_PASS: return "PASS";
    case CHECK_FAIL: return "FAIL";
    case CHECK_SKIP: return "SKIP";
    default:         return "????";
    }
}

static void report(const char *subsystem, check_result_t r, const char *fmt, ...)
{
    char detail[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);

    ESP_LOGI(TAG, "[ %s ] %-14s %s", result_str(r), subsystem, detail);
    switch (r) {
    case CHECK_PASS: s_pass_count++; break;
    case CHECK_FAIL: s_fail_count++; break;
    case CHECK_SKIP: s_skip_count++; break;
    }
}

// ===========================================================================
// Module state — REPL needs to mutate I2C pins/speed at runtime.
// ===========================================================================
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static int      s_i2c_sda_gpio = BOARD_I2C_SDA_GPIO;
static int      s_i2c_scl_gpio = BOARD_I2C_SCL_GPIO;
static uint32_t s_i2c_freq_hz  = 100000;          // bring-up starts slow; raise via "I2C 400"

static bool                s_spi_initialized = false;
static spi_device_handle_t s_lora_dev   = NULL;
static spi_device_handle_t s_cc1101_dev = NULL;

static TaskHandle_t  s_imu_stream_task = NULL;
static volatile bool s_imu_stream_run  = false;

static volatile bool s_ble_connected  = false;
static volatile bool s_ble_adv_active = false;

// ===========================================================================
// I2C bus management
// ===========================================================================
/* CITE: i2c_master_bus_config_t, i2c_new_master_bus, i2c_del_master_bus,
 *       i2c_master_bus_add_device, i2c_master_bus_rm_device,
 *       i2c_master_probe, i2c_master_transmit_receive, i2c_master_transmit
 *   docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2c.html
 */
static esp_err_t i2c_bus_open(void)
{
    if (s_i2c_bus != NULL) return ESP_OK;
    i2c_master_bus_config_t cfg = {
        .clk_source                  = I2C_CLK_SRC_DEFAULT,
        .i2c_port                    = BOARD_I2C_PORT,
        .scl_io_num                  = s_i2c_scl_gpio,
        .sda_io_num                  = s_i2c_sda_gpio,
        .glitch_ignore_cnt           = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c_bus);
}

static void i2c_bus_close(void)
{
    if (s_i2c_bus == NULL) return;
    i2c_del_master_bus(s_i2c_bus);
    s_i2c_bus = NULL;
}

static esp_err_t i2c_bus_reopen(void)
{
    i2c_bus_close();
    return i2c_bus_open();
}

/* Per-call device handle — pin/speed changes don't have to invalidate caches. */
static esp_err_t i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
    if (s_i2c_bus == NULL) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = s_i2c_freq_hz,
    };
    i2c_master_dev_handle_t dev;
    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dcfg, &dev);
    if (err != ESP_OK) return err;
    err = i2c_master_transmit_receive(dev, &reg, 1, buf, len, 100);
    i2c_master_bus_rm_device(dev);
    return err;
}

static esp_err_t i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
    if (s_i2c_bus == NULL) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = s_i2c_freq_hz,
    };
    i2c_master_dev_handle_t dev;
    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dcfg, &dev);
    if (err != ESP_OK) return err;
    uint8_t buf[2] = { reg, val };
    err = i2c_master_transmit(dev, buf, sizeof(buf), 100);
    i2c_master_bus_rm_device(dev);
    return err;
}

static int i2c_scan_inner(void)
{
    int count = 0;
    char list[160] = {0};
    size_t l = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(s_i2c_bus, a, 50) == ESP_OK) {
            l += snprintf(list + l, sizeof(list) - l, "%s0x%02X", l ? " " : "", a);
            if (a == BOARD_IMU_I2C_ADDR)  l += snprintf(list + l, sizeof(list) - l, "(IMU)");
            if (a == BOARD_FUEL_I2C_ADDR) l += snprintf(list + l, sizeof(list) - l, "(FUEL)");
            if (a == BOARD_BARO_I2C_ADDR) l += snprintf(list + l, sizeof(list) - l, "(BARO)");
            count++;
        }
    }
    if (count == 0) {
        printf("  no ACKs\n");
    } else {
        printf("  found %d device(s): %s\n", count, list);
    }
    return count;
}

// ===========================================================================
// MAX17048 fuel gauge
// ===========================================================================
/* CITE: MAX17048 register layout per Patrick's bench-validated bring-up sketch:
 *   0x02 VCELL   — 16-bit BE; 12-bit left-aligned; 1.25 mV/LSB after >> 4
 *   0x04 SOC     — high byte = % integer, low byte = % fraction / 256
 *   0x08 VERSION
 */
#define MAX17048_REG_VCELL    0x02u
#define MAX17048_REG_SOC      0x04u
#define MAX17048_REG_VERSION  0x08u

static esp_err_t fuel_read_u16_be(uint8_t reg, uint16_t *out)
{
    uint8_t b[2] = {0};
    esp_err_t err = i2c_read_reg(BOARD_FUEL_I2C_ADDR, reg, b, 2);
    if (err == ESP_OK) *out = ((uint16_t)b[0] << 8) | b[1];
    return err;
}

// ===========================================================================
// LSM6DSO32X IMU
// ===========================================================================
/* CITE: LSM6DSO32X register layout. FS_XL encoding on the DSO32X (verified
 * against STMicroelectronics/lsm6dso32x-pid lsm6dso32x_fs_xl_t) differs from
 * the regular LSM6DSO: 00=±4g, 01=±32g, 10=±8g, 11=±16g.
 *   0x0F WHO_AM_I            (expected 0x6C)
 *   0x12 CTRL3_C   = 0x44    (BDU=1, IF_INC=1)
 *   0x10 CTRL1_XL  = 0x40    (104 Hz, FS_XL=00 → ±4 g)
 *   0x11 CTRL2_G   = 0x44    (104 Hz, 500 dps)
 *   0x22..0x2D  gyro XYZ then accel XYZ (little-endian int16, 12 bytes)
 * Sensitivities: ±4g = 0.122 mg/LSB, 500 dps = 17.50 mdps/LSB.
 */
#define LSM6_REG_WHO_AM_I    0x0Fu
#define LSM6_REG_CTRL1_XL    0x10u
#define LSM6_REG_CTRL2_G     0x11u
#define LSM6_REG_CTRL3_C     0x12u
#define LSM6_REG_OUTX_L_G    0x22u
#define LSM6_WHOAMI_EXPECT   0x6Cu

#define LSM6_ACCEL_G_PER_LSB  (0.122f * 0.001f)   // ±4 g
#define LSM6_GYRO_DPS_PER_LSB (17.50f * 0.001f)   // 500 dps

static int16_t le16(const uint8_t *b)
{
    return (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

// ===========================================================================
// SPI bus (LoRa + CC1101 share)
// ===========================================================================
static esp_err_t spi_bus_ensure(void)
{
    if (s_spi_initialized) return ESP_OK;
    spi_bus_config_t bus = {
        .miso_io_num     = BOARD_SPI_MISO_GPIO,
        .mosi_io_num     = BOARD_SPI_MOSI_GPIO,
        .sclk_io_num     = BOARD_SPI_SCK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 64,
    };
    esp_err_t err = spi_bus_initialize((spi_host_device_t)BOARD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_spi_initialized = true;
        return ESP_OK;
    }
    return err;
}

static esp_err_t spi_attach_lora(void)
{
    if (s_lora_dev != NULL) return ESP_OK;
    esp_err_t err = spi_bus_ensure();
    if (err != ESP_OK) return err;
    /* .ino uses 8 MHz; SX127x family supports up to 10 MHz. */
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 8 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = BOARD_LORA_CS_GPIO,
        .queue_size     = 1,
    };
    return spi_bus_add_device((spi_host_device_t)BOARD_SPI_HOST, &dev, &s_lora_dev);
}

static esp_err_t spi_attach_cc1101(void)
{
    if (s_cc1101_dev != NULL) return ESP_OK;
    esp_err_t err = spi_bus_ensure();
    if (err != ESP_OK) return err;
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = BOARD_CC1101_CS_GPIO,
        .queue_size     = 1,
    };
    return spi_bus_add_device((spi_host_device_t)BOARD_SPI_HOST, &dev, &s_cc1101_dev);
}

// ===========================================================================
// RFM95W / SX1276
// ===========================================================================
/* CITE: SX127x family RegVersion = 0x42, value 0x12. SPI protocol: bit 7 of
 * first header byte = W/R (1 = write), bits 6:0 = register address.
 * Bench-validated by Patrick's .ino. */
#define RFM95_REG_VERSION    0x42u
#define RFM95_VERSION_EXPECT 0x12u

static esp_err_t lora_read_reg(uint8_t reg, uint8_t *out)
{
    if (s_lora_dev == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t tx[2] = { (uint8_t)(reg & 0x7Fu), 0x00 };
    uint8_t rx[2] = {0};
    spi_transaction_t t = {
        .length    = 8 * sizeof(tx),
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_polling_transmit(s_lora_dev, &t);
    if (err == ESP_OK) *out = rx[1];
    return err;
}

// ===========================================================================
// CC1101
// ===========================================================================
/* CITE: CC1101 SPI header: bit 7 = R/W (1 = read), bit 6 = burst, bits 5:0 =
 * register address. Status registers (>= 0x30) require burst bit set.
 *   PARTNUM=0x30  VERSION=0x31  MARCSTATE=0x35  SRES strobe = 0x30
 * From .ino + TI CC1101 datasheet. */
static esp_err_t cc1101_read_reg(uint8_t addr, uint8_t *out)
{
    if (s_cc1101_dev == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t header = (uint8_t)(addr | 0x80u);
    if (addr >= 0x30) header |= 0x40u;
    uint8_t tx[2] = { header, 0x00 };
    uint8_t rx[2] = {0};
    spi_transaction_t t = {
        .length    = 8 * sizeof(tx),
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    esp_err_t err = spi_device_polling_transmit(s_cc1101_dev, &t);
    if (err == ESP_OK) *out = rx[1];
    return err;
}

static esp_err_t cc1101_strobe(uint8_t cmd)
{
    if (s_cc1101_dev == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t tx[1] = { cmd };
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = tx,
    };
    return spi_device_polling_transmit(s_cc1101_dev, &t);
}

// ===========================================================================
// REPL command implementations
// ===========================================================================
static void cmd_i2c_scan(void)
{
    if (s_i2c_bus == NULL) { printf("I2C bus not initialized\n"); return; }
    printf("I2C scan SDA=IO%d SCL=IO%d @ %lu Hz\n",
           s_i2c_sda_gpio, s_i2c_scl_gpio, (unsigned long)s_i2c_freq_hz);
    i2c_scan_inner();
}

static void cmd_i2c_levels(void)
{
    bool was_open = (s_i2c_bus != NULL);
    if (was_open) i2c_bus_close();

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << s_i2c_sda_gpio) | (1ULL << s_i2c_scl_gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    esp_rom_delay_us(10000);
    int sda = gpio_get_level(s_i2c_sda_gpio);
    int scl = gpio_get_level(s_i2c_scl_gpio);
    printf("I2C levels: SDA=IO%d=%d SCL=IO%d=%d (idle expect both 1)\n",
           s_i2c_sda_gpio, sda, s_i2c_scl_gpio, scl);

    if (was_open) i2c_bus_open();
}

static void cmd_i2c_recover(void)
{
    bool was_open = (s_i2c_bus != NULL);
    if (was_open) i2c_bus_close();

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << s_i2c_sda_gpio) | (1ULL << s_i2c_scl_gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    esp_rom_delay_us(2000);

    /* Clock SCL up to 18 times to release a slave stuck holding SDA low. */
    for (int i = 0; i < 18; i++) {
        gpio_set_direction(s_i2c_scl_gpio, GPIO_MODE_OUTPUT_OD);
        gpio_set_level(s_i2c_scl_gpio, 0);
        esp_rom_delay_us(8);
        gpio_set_direction(s_i2c_scl_gpio, GPIO_MODE_INPUT);
        esp_rom_delay_us(8);
    }

    /* STOP: SDA low while SCL high, then release SDA. */
    gpio_set_direction(s_i2c_sda_gpio, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(s_i2c_sda_gpio, 0);
    esp_rom_delay_us(8);
    gpio_set_direction(s_i2c_scl_gpio, GPIO_MODE_INPUT);
    esp_rom_delay_us(8);
    gpio_set_direction(s_i2c_sda_gpio, GPIO_MODE_INPUT);
    esp_rom_delay_us(5000);

    printf("I2C recovery done: SDA=%d SCL=%d (expect both 1)\n",
           gpio_get_level(s_i2c_sda_gpio), gpio_get_level(s_i2c_scl_gpio));

    if (was_open) i2c_bus_open();
}

static void cmd_i2c_find(void)
{
    printf("I2C FIND: trying normal/swapped IO%d/IO%d at 100k and 400k...\n",
           BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
    printf("Expected: MAX17048=0x%02X, LSM6DSO32X=0x%02X\n",
           BOARD_FUEL_I2C_ADDR, BOARD_IMU_I2C_ADDR);

    struct { int sda; int scl; uint32_t hz; const char *label; } trials[] = {
        { BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO, 100000, "NORMAL 100k" },
        { BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO, 400000, "NORMAL 400k" },
        { BOARD_I2C_SCL_GPIO, BOARD_I2C_SDA_GPIO, 100000, "SWAP   100k" },
        { BOARD_I2C_SCL_GPIO, BOARD_I2C_SDA_GPIO, 400000, "SWAP   400k" },
    };
    bool any_found = false;
    for (size_t i = 0; i < sizeof(trials)/sizeof(trials[0]); i++) {
        i2c_bus_close();
        s_i2c_sda_gpio = trials[i].sda;
        s_i2c_scl_gpio = trials[i].scl;
        s_i2c_freq_hz  = trials[i].hz;
        printf("\n%s SDA=IO%d SCL=IO%d\n", trials[i].label,
               trials[i].sda, trials[i].scl);
        esp_err_t err = i2c_bus_open();
        if (err != ESP_OK) {
            printf("  bus open failed: %s\n", esp_err_to_name(err));
            continue;
        }
        if (i2c_scan_inner() > 0) any_found = true;
    }

    // Restore defaults.
    i2c_bus_close();
    s_i2c_sda_gpio = BOARD_I2C_SDA_GPIO;
    s_i2c_scl_gpio = BOARD_I2C_SCL_GPIO;
    s_i2c_freq_hz  = 100000;
    i2c_bus_open();
    printf("\nReset to NORMAL SDA=IO%d SCL=IO%d @ 100 kHz\n",
           s_i2c_sda_gpio, s_i2c_scl_gpio);
    if (!any_found) {
        printf("I2C FIND: no devices found. Check 3.3 V at U3/U6 and PCB pull-ups.\n");
    }
}

static void cmd_i2c_pins_normal(void)
{
    s_i2c_sda_gpio = BOARD_I2C_SDA_GPIO;
    s_i2c_scl_gpio = BOARD_I2C_SCL_GPIO;
    i2c_bus_reopen();
    printf("I2C pins NORMAL: SDA=IO%d SCL=IO%d\n", s_i2c_sda_gpio, s_i2c_scl_gpio);
}

static void cmd_i2c_pins_swap(void)
{
    s_i2c_sda_gpio = BOARD_I2C_SCL_GPIO;
    s_i2c_scl_gpio = BOARD_I2C_SDA_GPIO;
    i2c_bus_reopen();
    printf("I2C pins SWAPPED (diagnostic): SDA=IO%d SCL=IO%d\n",
           s_i2c_sda_gpio, s_i2c_scl_gpio);
}

static void cmd_i2c_speed(uint32_t hz)
{
    s_i2c_freq_hz = hz;
    printf("I2C speed set to %lu Hz (applied on next device transaction)\n",
           (unsigned long)hz);
}

static void cmd_fuel(void)
{
    if (s_i2c_bus == NULL) { printf("FUEL: I2C bus not initialized\n"); return; }
    uint16_t vraw = 0, soc_raw = 0, ver = 0;
    esp_err_t e1 = fuel_read_u16_be(MAX17048_REG_VCELL,   &vraw);
    esp_err_t e2 = fuel_read_u16_be(MAX17048_REG_SOC,     &soc_raw);
    esp_err_t e3 = fuel_read_u16_be(MAX17048_REG_VERSION, &ver);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        printf("FUEL: MAX17048 @ 0x%02X not responding (vcell=%s soc=%s)\n",
               BOARD_FUEL_I2C_ADDR, esp_err_to_name(e1), esp_err_to_name(e2));
        return;
    }
    float vbat = (float)(vraw >> 4) * 0.00125f;
    float soc  = (float)(soc_raw >> 8) + ((float)(soc_raw & 0xFF) / 256.0f);
    int alrt = gpio_get_level(BOARD_FUEL_ALRT_GPIO);
    printf("FUEL: VBAT=%.3f V  SOC=%.2f %%  ALRT_N=%d", vbat, soc, alrt);
    if (e3 == ESP_OK) printf("  VERSION=0x%04X", ver);
    printf("\n");
}

static void cmd_imu_init(void)
{
    if (s_i2c_bus == NULL) { printf("IMU: I2C bus not initialized\n"); return; }
    uint8_t who = 0;
    esp_err_t err = i2c_read_reg(BOARD_IMU_I2C_ADDR, LSM6_REG_WHO_AM_I, &who, 1);
    if (err != ESP_OK) {
        printf("IMU: not responding at 0x%02X (%s)\n",
               BOARD_IMU_I2C_ADDR, esp_err_to_name(err));
        return;
    }
    i2c_write_reg(BOARD_IMU_I2C_ADDR, LSM6_REG_CTRL3_C,  0x44);
    i2c_write_reg(BOARD_IMU_I2C_ADDR, LSM6_REG_CTRL1_XL, 0x40);
    i2c_write_reg(BOARD_IMU_I2C_ADDR, LSM6_REG_CTRL2_G,  0x44);
    printf("IMU: WHO_AM_I=0x%02X (expect 0x%02X). Init: 104 Hz ±4g / 500 dps.\n",
           who, LSM6_WHOAMI_EXPECT);
}

static void cmd_imu_read_once(void)
{
    if (s_i2c_bus == NULL) { printf("IMU: I2C bus not initialized\n"); return; }
    uint8_t who = 0;
    if (i2c_read_reg(BOARD_IMU_I2C_ADDR, LSM6_REG_WHO_AM_I, &who, 1) != ESP_OK) {
        printf("IMU: not responding at 0x%02X\n", BOARD_IMU_I2C_ADDR);
        return;
    }
    uint8_t b[12];
    if (i2c_read_reg(BOARD_IMU_I2C_ADDR, LSM6_REG_OUTX_L_G, b, 12) != ESP_OK) {
        printf("IMU: data read failed\n");
        return;
    }
    int16_t gx = le16(&b[0]), gy = le16(&b[2]), gz = le16(&b[4]);
    int16_t ax = le16(&b[6]), ay = le16(&b[8]), az = le16(&b[10]);
    printf("IMU WHO=0x%02X INT1=%d INT2=%d | "
           "A[g] x=%+.3f y=%+.3f z=%+.3f | "
           "G[dps] x=%+.2f y=%+.2f z=%+.2f\n",
           who,
           gpio_get_level(BOARD_IMU_INT1_GPIO),
           gpio_get_level(BOARD_IMU_INT2_GPIO),
           ax * LSM6_ACCEL_G_PER_LSB,
           ay * LSM6_ACCEL_G_PER_LSB,
           az * LSM6_ACCEL_G_PER_LSB,
           gx * LSM6_GYRO_DPS_PER_LSB,
           gy * LSM6_GYRO_DPS_PER_LSB,
           gz * LSM6_GYRO_DPS_PER_LSB);
}

static void imu_stream_task_fn(void *arg)
{
    while (s_imu_stream_run) {
        cmd_imu_read_once();
        vTaskDelay(pdMS_TO_TICKS(50));    // 20 Hz target
    }
    s_imu_stream_task = NULL;
    vTaskDelete(NULL);
}

static void cmd_imu_stream_on(void)
{
    if (s_imu_stream_task != NULL) { printf("IMU streaming already ON\n"); return; }
    s_imu_stream_run = true;
    BaseType_t ok = xTaskCreate(imu_stream_task_fn, "imu_stream",
                                4096, NULL, 5, &s_imu_stream_task);
    if (ok != pdPASS) {
        s_imu_stream_run = false;
        s_imu_stream_task = NULL;
        printf("IMU stream: xTaskCreate failed\n");
        return;
    }
    printf("IMU streaming ON (20 Hz). Send 'IMU STREAM OFF' to stop.\n");
}

static void cmd_imu_stream_off(void)
{
    s_imu_stream_run = false;
    printf("IMU streaming OFF (task exits on next tick)\n");
}

static void cmd_lora_version(void)
{
    if (spi_attach_lora() != ESP_OK) { printf("LoRa: SPI attach failed\n"); return; }
    uint8_t v = 0;
    if (lora_read_reg(RFM95_REG_VERSION, &v) != ESP_OK) {
        printf("LoRa: read failed\n");
        return;
    }
    printf("LoRa/RFM95: RegVersion=0x%02X %s  DIO0=%d DIO1=%d\n",
           v, (v == RFM95_VERSION_EXPECT ? "OK" : "UNEXPECTED"),
           gpio_get_level(BOARD_LORA_DIO0_GPIO),
           gpio_get_level(BOARD_LORA_DIO1_GPIO));
}

static void cmd_lora_reset(void)
{
    /* RFM95 reset is often open-drain style; toggle direction to release. */
    gpio_set_direction(BOARD_LORA_RST_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_LORA_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_direction(BOARD_LORA_RST_GPIO, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_direction(BOARD_LORA_RST_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_LORA_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    printf("LoRa: reset pulse complete\n");
}

static void cmd_cc1101_id(void)
{
    if (spi_attach_cc1101() != ESP_OK) { printf("CC1101: SPI attach failed\n"); return; }
    uint8_t partnum = 0, version = 0, marcstate = 0;
    cc1101_read_reg(0x30, &partnum);
    cc1101_read_reg(0x31, &version);
    cc1101_read_reg(0x35, &marcstate);
    printf("CC1101: PARTNUM=0x%02X VERSION=0x%02X MARCSTATE=0x%02X "
           "GDO0=%d GDO2=%d\n",
           partnum, version, marcstate,
           gpio_get_level(BOARD_CC1101_GDO0_GPIO),
           gpio_get_level(BOARD_CC1101_GDO2_GPIO));
}

static void cmd_cc1101_reset(void)
{
    if (spi_attach_cc1101() != ESP_OK) { printf("CC1101: SPI attach failed\n"); return; }
    gpio_set_direction(BOARD_CC1101_CS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_CC1101_CS_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(BOARD_CC1101_CS_GPIO, 0);
    esp_rom_delay_us(20);
    gpio_set_level(BOARD_CC1101_CS_GPIO, 1);
    esp_rom_delay_us(40);
    cc1101_strobe(0x30);   // SRES
    vTaskDelay(pdMS_TO_TICKS(5));
    printf("CC1101: SRES strobe sent\n");
}

static void cmd_gps_read(uint32_t ms)
{
    const uart_port_t port = (uart_port_t)BOARD_GPS_UART_PORT;
    uart_config_t cfg = {
        .baud_rate  = BOARD_GPS_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(port, 2048, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("GPS: uart_driver_install -> %s\n", esp_err_to_name(err));
        return;
    }
    uart_param_config(port, &cfg);
    uart_set_pin(port, BOARD_GPS_UART_TX_GPIO, BOARD_GPS_UART_RX_GPIO,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    printf("GPS read %lu ms (UART%d TX=IO%d RX=IO%d @ %d 8N1)  PPS=%d\n",
           (unsigned long)ms, BOARD_GPS_UART_PORT,
           BOARD_GPS_UART_TX_GPIO, BOARD_GPS_UART_RX_GPIO,
           BOARD_GPS_UART_BAUD,
           gpio_get_level(BOARD_GPS_PPS_GPIO));

    uint8_t buf[256];
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    while (xTaskGetTickCount() < deadline) {
        int n = uart_read_bytes(port, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n > 0) {
            fwrite(buf, 1, n, stdout);
            fflush(stdout);
        }
    }
    printf("\n");
    uart_driver_delete(port);
}

static void cmd_gps_reset(void)
{
    gpio_set_direction(BOARD_GPS_RESET_N_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_GPS_RESET_N_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOARD_GPS_RESET_N_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    printf("GPS: RESET_N pulse complete\n");
}

static void cmd_gps_safeboot(bool assert_low)
{
    gpio_set_direction(BOARD_GPS_SAFEBOOT_N_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_GPS_SAFEBOOT_N_GPIO, assert_low ? 0 : 1);
    printf("GPS SAFEBOOT_N %s\n", assert_low ? "asserted LOW" : "released HIGH");
}

static void cmd_chg(bool enable)
{
    gpio_set_direction(BOARD_CHARGER_EN_N_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_CHARGER_EN_N_GPIO, enable ? 0 : 1);  // CE active low
    printf("Charger %s: CHGR-EN-N driven %s\n",
           enable ? "ENABLED" : "DISABLED",
           enable ? "LOW" : "HIGH");
}

static void cmd_pins(void)
{
    printf("Pin map (board.h):\n");
    printf("  I2C:    SDA=IO%d SCL=IO%d\n", BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
    printf("  IMU:    INT1=IO%d INT2=IO%d  addr=0x%02X\n",
           BOARD_IMU_INT1_GPIO, BOARD_IMU_INT2_GPIO, BOARD_IMU_I2C_ADDR);
    printf("  Fuel:   ALRT_N=IO%d  addr=0x%02X\n",
           BOARD_FUEL_ALRT_GPIO, BOARD_FUEL_I2C_ADDR);
    printf("  Chgr:   CE/EN_N=IO%d (active low)\n", BOARD_CHARGER_EN_N_GPIO);
    printf("  SPI:    SCK=IO%d MOSI=IO%d MISO=IO%d\n",
           BOARD_SPI_SCK_GPIO, BOARD_SPI_MOSI_GPIO, BOARD_SPI_MISO_GPIO);
    printf("  LoRa:   CS=IO%d RST=IO%d DIO0=IO%d DIO1=IO%d\n",
           BOARD_LORA_CS_GPIO, BOARD_LORA_RST_GPIO,
           BOARD_LORA_DIO0_GPIO, BOARD_LORA_DIO1_GPIO);
    printf("  CC1101: CS=IO%d GDO0=IO%d GDO2=IO%d\n",
           BOARD_CC1101_CS_GPIO, BOARD_CC1101_GDO0_GPIO, BOARD_CC1101_GDO2_GPIO);
    printf("  GPS:    TX=IO%d(->RXD)  RX=IO%d(<-TXD)  "
           "PPS=IO%d  RST_N=IO%d  SAFEBOOT_N=IO%d\n",
           BOARD_GPS_UART_TX_GPIO, BOARD_GPS_UART_RX_GPIO,
           BOARD_GPS_PPS_GPIO, BOARD_GPS_RESET_N_GPIO, BOARD_GPS_SAFEBOOT_N_GPIO);
}

static void cmd_status(void)
{
    printf("Status:\n");
    printf("  I2C active: SDA=IO%d SCL=IO%d @ %lu Hz  bus=%s\n",
           s_i2c_sda_gpio, s_i2c_scl_gpio, (unsigned long)s_i2c_freq_hz,
           s_i2c_bus ? "open" : "closed");
    printf("  CHGR_EN_N=%d  FUEL_ALRT_N=%d\n",
           gpio_get_level(BOARD_CHARGER_EN_N_GPIO),
           gpio_get_level(BOARD_FUEL_ALRT_GPIO));
    printf("  GPS  PPS=%d  RESET_N=%d  SAFEBOOT_N=%d\n",
           gpio_get_level(BOARD_GPS_PPS_GPIO),
           gpio_get_level(BOARD_GPS_RESET_N_GPIO),
           gpio_get_level(BOARD_GPS_SAFEBOOT_N_GPIO));
    printf("  IMU  INT1=%d INT2=%d   streaming=%s\n",
           gpio_get_level(BOARD_IMU_INT1_GPIO),
           gpio_get_level(BOARD_IMU_INT2_GPIO),
           s_imu_stream_task ? "ON" : "OFF");
    printf("  LoRa CS=%d RST=%d DIO0=%d DIO1=%d\n",
           gpio_get_level(BOARD_LORA_CS_GPIO),
           gpio_get_level(BOARD_LORA_RST_GPIO),
           gpio_get_level(BOARD_LORA_DIO0_GPIO),
           gpio_get_level(BOARD_LORA_DIO1_GPIO));
    printf("  CC1101 CS=%d GDO0=%d GDO2=%d\n",
           gpio_get_level(BOARD_CC1101_CS_GPIO),
           gpio_get_level(BOARD_CC1101_GDO0_GPIO),
           gpio_get_level(BOARD_CC1101_GDO2_GPIO));
    cmd_fuel();
    cmd_lora_version();
    cmd_cc1101_id();
}

static void cmd_help(void)
{
    printf("\nCommands:\n");
    printf("  HELP                  Show this menu\n");
    printf("  STATUS                Print pin states + quick device status\n");
    printf("  PINS                  Print schematic-derived MCU pin map\n");
    printf("  I2C                   Scan I2C bus (or: SCAN)\n");
    printf("  I2C LEVELS            Print SDA/SCL idle levels\n");
    printf("  I2C RECOVER           Clock-pulse to release stuck slave\n");
    printf("  I2C FIND              Try normal/swapped at 100k/400k\n");
    printf("  I2C PINS NORMAL       SDA=IO%d SCL=IO%d (board.h truth)\n",
           BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
    printf("  I2C PINS SWAP         Diagnostic pin swap\n");
    printf("  I2C 100 | I2C 400     Set I2C speed (kHz)\n");
    printf("  FUEL                  Read MAX17048 VBAT/SOC/VERSION (or: BAT)\n");
    printf("  IMU INIT              Configure LSM6DSO32X 104 Hz ±4g / 500 dps\n");
    printf("  IMU READ              Read accel/gyro once\n");
    printf("  IMU STREAM ON|OFF     Stream IMU at 20 Hz\n");
    printf("  GPS READ [ms]         Dump GPS UART for ms (default 3000)\n");
    printf("  GPS RESET             Pulse GPS RESET_N\n");
    printf("  GPS SAFEBOOT ON|OFF   Assert/release SAFEBOOT_N (active low)\n");
    printf("  LORA VER              Read RFM95 RegVersion (expect 0x%02X)\n",
           RFM95_VERSION_EXPECT);
    printf("  LORA RESET            Pulse RFM95 reset\n");
    printf("  CC1101 ID             Read PARTNUM/VERSION/MARCSTATE\n");
    printf("  CC1101 RESET          SRES strobe\n");
    printf("  CHG EN | CHG DIS      Drive CHGR-EN-N low/high\n");
}

// ===========================================================================
// One-shot walk (runs on boot before dropping into REPL)
// ===========================================================================
static check_result_t check_nvs(void)
{
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

static check_result_t check_i2c_bus(void)
{
    esp_err_t err = i2c_bus_open();
    if (err != ESP_OK) {
        report("i2c_init", CHECK_FAIL,
               "i2c_new_master_bus -> %s (port=%d sda=%d scl=%d)",
               esp_err_to_name(err), BOARD_I2C_PORT,
               s_i2c_sda_gpio, s_i2c_scl_gpio);
        return CHECK_FAIL;
    }
    report("i2c_init", CHECK_PASS,
           "port=%d sda=%d scl=%d freq=%lu Hz",
           BOARD_I2C_PORT, s_i2c_sda_gpio, s_i2c_scl_gpio,
           (unsigned long)s_i2c_freq_hz);
    return CHECK_PASS;
}

static check_result_t check_i2c_scan(void)
{
    if (s_i2c_bus == NULL) {
        report("i2c_scan", CHECK_SKIP, "I2C bus not initialized");
        return CHECK_SKIP;
    }
    bool saw_imu = false, saw_fuel = false, saw_baro = false;
    int found = 0;
    char list[128] = {0};
    size_t l = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(s_i2c_bus, a, 50) == ESP_OK) {
            found++;
            l += snprintf(list + l, sizeof(list) - l, "%s0x%02X", l ? " " : "", a);
            if (a == BOARD_IMU_I2C_ADDR)  saw_imu  = true;
            if (a == BOARD_FUEL_I2C_ADDR) saw_fuel = true;
            if (a == BOARD_BARO_I2C_ADDR) saw_baro = true;
        }
    }
    ESP_LOGI(TAG, "i2c_scan: found %d device(s) [%s]", found, list);

    if (saw_imu && saw_fuel) {
        report("i2c_scan", CHECK_PASS,
               "imu=0x%02X fuel=0x%02X baro=%s",
               BOARD_IMU_I2C_ADDR, BOARD_FUEL_I2C_ADDR,
               saw_baro ? "present" : "DNP");
        return CHECK_PASS;
    }
    report("i2c_scan", CHECK_FAIL, "missing: %s%s",
           saw_imu  ? "" : "IMU ", saw_fuel ? "" : "FUEL ");
    return CHECK_FAIL;
}

static check_result_t check_imu_whoami(void)
{
    if (s_i2c_bus == NULL) {
        report("imu", CHECK_SKIP, "I2C bus not initialized");
        return CHECK_SKIP;
    }
    uint8_t who = 0;
    esp_err_t err = i2c_read_reg(BOARD_IMU_I2C_ADDR, LSM6_REG_WHO_AM_I, &who, 1);
    if (err != ESP_OK) {
        report("imu", CHECK_FAIL, "WHO_AM_I read failed: %s", esp_err_to_name(err));
        return CHECK_FAIL;
    }
    if (who == LSM6_WHOAMI_EXPECT) {
        report("imu", CHECK_PASS, "LSM6DSO32X @ 0x%02X WHO_AM_I=0x%02X",
               BOARD_IMU_I2C_ADDR, who);
        return CHECK_PASS;
    }
    report("imu", CHECK_FAIL,
           "WHO_AM_I=0x%02X (expected 0x%02X)", who, LSM6_WHOAMI_EXPECT);
    return CHECK_FAIL;
}

static check_result_t check_fuel(void)
{
    if (s_i2c_bus == NULL) {
        report("fuel", CHECK_SKIP, "I2C bus not initialized");
        return CHECK_SKIP;
    }
    uint16_t vraw = 0, soc_raw = 0;
    if (fuel_read_u16_be(MAX17048_REG_VCELL, &vraw)   != ESP_OK ||
        fuel_read_u16_be(MAX17048_REG_SOC,   &soc_raw) != ESP_OK) {
        report("fuel", CHECK_FAIL, "MAX17048 @ 0x%02X read failed",
               BOARD_FUEL_I2C_ADDR);
        return CHECK_FAIL;
    }
    float vbat = (float)(vraw >> 4) * 0.00125f;
    float soc  = (float)(soc_raw >> 8) + ((float)(soc_raw & 0xFF) / 256.0f);
    report("fuel", CHECK_PASS, "VBAT=%.3f V SOC=%.2f%%", vbat, soc);
    return CHECK_PASS;
}

static check_result_t check_charger_en(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_CHARGER_EN_N_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        report("charger", CHECK_FAIL, "gpio_config(CE) failed");
        return CHECK_FAIL;
    }
    gpio_set_level(BOARD_CHARGER_EN_N_GPIO, 0);  // CE low = charger ENABLED
    report("charger", CHECK_PASS,
           "CE/CHGR-EN-N=GPIO%d driven LOW (charger ENABLED at boot)",
           BOARD_CHARGER_EN_N_GPIO);
    return CHECK_PASS;
}

static check_result_t check_lora_version(void)
{
#if !BOARD_HAS_LORA
    report("lora", CHECK_SKIP, "BOARD_HAS_LORA == 0");
    return CHECK_SKIP;
#else
    if (spi_attach_lora() != ESP_OK) {
        report("lora", CHECK_FAIL, "SPI attach failed");
        return CHECK_FAIL;
    }
    uint8_t v = 0;
    if (lora_read_reg(RFM95_REG_VERSION, &v) != ESP_OK) {
        report("lora", CHECK_FAIL, "RegVersion read failed");
        return CHECK_FAIL;
    }
    if (v == RFM95_VERSION_EXPECT) {
        report("lora", CHECK_PASS, "RFM95 RegVersion=0x%02X", v);
        return CHECK_PASS;
    }
    report("lora", CHECK_FAIL,
           "RFM95 RegVersion=0x%02X (expected 0x%02X)", v, RFM95_VERSION_EXPECT);
    return CHECK_FAIL;
#endif
}

static check_result_t check_cc1101_id(void)
{
#if !BOARD_HAS_SUBGHZ_RADIO
    report("cc1101", CHECK_SKIP, "BOARD_HAS_SUBGHZ_RADIO == 0");
    return CHECK_SKIP;
#else
    if (spi_attach_cc1101() != ESP_OK) {
        report("cc1101", CHECK_FAIL, "SPI attach failed");
        return CHECK_FAIL;
    }
    uint8_t partnum = 0, version = 0;
    if (cc1101_read_reg(0x30, &partnum) != ESP_OK ||
        cc1101_read_reg(0x31, &version) != ESP_OK) {
        report("cc1101", CHECK_FAIL, "PARTNUM/VERSION read failed");
        return CHECK_FAIL;
    }
    /* 0xFF on both bytes typically means MISO floated — no slave responding. */
    if (partnum == 0xFF && version == 0xFF) {
        report("cc1101", CHECK_FAIL,
               "PARTNUM=0xFF VERSION=0xFF (MISO floating — DNP or wiring?)");
        return CHECK_FAIL;
    }
    report("cc1101", CHECK_PASS,
           "PARTNUM=0x%02X VERSION=0x%02X", partnum, version);
    return CHECK_PASS;
#endif
}

static check_result_t check_gps(void)
{
#if !BOARD_HAS_GPS
    report("gps", CHECK_SKIP, "BOARD_HAS_GPS == 0");
    return CHECK_SKIP;
#else
    const uart_port_t port = (uart_port_t)BOARD_GPS_UART_PORT;
    uart_config_t cfg = {
        .baud_rate  = BOARD_GPS_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(port, 2048, 0, 0, NULL, 0) != ESP_OK ||
        uart_param_config(port, &cfg) != ESP_OK ||
        uart_set_pin(port, BOARD_GPS_UART_TX_GPIO, BOARD_GPS_UART_RX_GPIO,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        report("gps", CHECK_FAIL, "uart setup failed");
        uart_driver_delete(port);
        return CHECK_FAIL;
    }

    /* 3 s window — first "$G" byte pair is the PASS signal. The REPL's
     * GPS READ command dumps raw NMEA for full inspection. */
    uint8_t buf[256];
    bool saw_dollar_g = false;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    while (xTaskGetTickCount() < deadline && !saw_dollar_g) {
        int n = uart_read_bytes(port, buf, sizeof(buf), pdMS_TO_TICKS(200));
        for (int i = 0; i + 1 < n; i++) {
            if (buf[i] == '$' && buf[i+1] == 'G') { saw_dollar_g = true; break; }
        }
    }
    uart_driver_delete(port);

    if (saw_dollar_g) {
        report("gps", CHECK_PASS,
               "NMEA seen on UART%d TX=IO%d RX=IO%d",
               BOARD_GPS_UART_PORT, BOARD_GPS_UART_TX_GPIO, BOARD_GPS_UART_RX_GPIO);
        return CHECK_PASS;
    }
    report("gps", CHECK_FAIL,
           "no NMEA in 3 s on UART%d TX=IO%d RX=IO%d (try REPL: GPS READ 5000)",
           BOARD_GPS_UART_PORT, BOARD_GPS_UART_TX_GPIO, BOARD_GPS_UART_RX_GPIO);
    return CHECK_FAIL;
#endif
}

// ---- BLE (advertise + library-eval harness GATT service) ------------------
//
// The harness registers the four contract characteristics from
// ../onecollar-platform/contracts/ble_protocol.md §GATT layout against the
// NimBLE host stack. It exercises gates 1, 2, 4, 5 of the mobile-library
// eval (see ../onecollar-platform/docs/11z-ble-library-eval.md) — MTU
// negotiation, per-characteristic CCCD independence, LESC bonding
// persistence, envelope passthrough. Gate 3 (TimeSync RTT precision)
// needs nanopb runtime + the generated bindings linked in, which is a
// follow-up commit.
//
// Wired into check_ble(); does NOT touch production drivers/ble_service.c.

// UUIDs are little-endian in BLE_UUID128_INIT — bytes reversed from the
// human-readable form in the contract.
//   Service:   0cbe0000-1000-4001-a000-000000000001
//   cmd_rx:    0cbe0001-...                (write, write-no-rsp)
//   cmd_tx:    0cbe0002-...                (notify)
//   event_tx:  0cbe0003-...                (notify)
//   stream_tx: 0cbe0004-...                (notify)
static const ble_uuid128_t harness_svc_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0,
    0x01, 0x40, 0x00, 0x10, 0x00, 0x00, 0xbe, 0x0c);
static const ble_uuid128_t harness_cmd_rx_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0,
    0x01, 0x40, 0x00, 0x10, 0x01, 0x00, 0xbe, 0x0c);
static const ble_uuid128_t harness_cmd_tx_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0,
    0x01, 0x40, 0x00, 0x10, 0x02, 0x00, 0xbe, 0x0c);
static const ble_uuid128_t harness_event_tx_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0,
    0x01, 0x40, 0x00, 0x10, 0x03, 0x00, 0xbe, 0x0c);
static const ble_uuid128_t harness_stream_tx_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0,
    0x01, 0x40, 0x00, 0x10, 0x04, 0x00, 0xbe, 0x0c);

static uint16_t harness_cmd_tx_handle;
static uint16_t harness_event_tx_handle;
static uint16_t harness_stream_tx_handle;
static uint16_t harness_conn_handle = BLE_HS_CONN_HANDLE_NONE;

#define HARNESS_FRAME_MAX 600  /* MTU 512 - ATT(3) = 509 max ATT payload; round up */

// cmd_rx write callback. Gate 5: echo the bytes back on cmd_tx unchanged so
// the Flutter harness can assert bit-identical receive across the platform.
// Other characteristics are notify-only and don't accept writes.
static int harness_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0;
    }
    uint8_t buf[HARNESS_FRAME_MAX];
    uint16_t out_len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &out_len);
    if (rc != 0) {
        ESP_LOGW(TAG, "harness: cmd_rx flatten rc=%d", rc);
        return rc;
    }
    if (harness_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "harness: cmd_rx write with no active conn; dropping");
        return 0;
    }
    struct os_mbuf *notif = ble_hs_mbuf_from_flat(buf, out_len);
    if (notif == NULL) {
        ESP_LOGE(TAG, "harness: cmd_tx mbuf alloc failed (%u bytes)", out_len);
        return BLE_HS_ENOMEM;
    }
    rc = ble_gatts_notify_custom(harness_conn_handle, harness_cmd_tx_handle, notif);
    if (rc != 0) {
        ESP_LOGW(TAG, "harness: cmd_tx notify rc=%d", rc);
    }
    return 0;
}

static const struct ble_gatt_chr_def harness_chrs[] = {
    {
        .uuid = &harness_cmd_rx_uuid.u,
        .access_cb = harness_chr_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &harness_cmd_tx_uuid.u,
        .access_cb = harness_chr_access,
        .val_handle = &harness_cmd_tx_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY,
    },
    {
        .uuid = &harness_event_tx_uuid.u,
        .access_cb = harness_chr_access,
        .val_handle = &harness_event_tx_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY,
    },
    {
        .uuid = &harness_stream_tx_uuid.u,
        .access_cb = harness_chr_access,
        .val_handle = &harness_stream_tx_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY,
    },
    { 0 },
};

static const struct ble_gatt_svc_def harness_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &harness_svc_uuid.u,
        .characteristics = harness_chrs,
    },
    { 0 },
};

static int bringup_ble_harness_register(void)
{
    int rc = ble_gatts_count_cfg(harness_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "harness: ble_gatts_count_cfg rc=%d", rc);
        return rc;
    }
    rc = ble_gatts_add_svcs(harness_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "harness: ble_gatts_add_svcs rc=%d", rc);
        return rc;
    }
    ESP_LOGI(TAG, "harness: GATT service + 4 chars registered "
                  "(cmd_tx=0x%04x event_tx=0x%04x stream_tx=0x%04x)",
             harness_cmd_tx_handle, harness_event_tx_handle, harness_stream_tx_handle);
    return 0;
}

static int bringup_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "ble: connect %s status=%d",
                 event->connect.status == 0 ? "ESTABLISHED" : "FAILED",
                 event->connect.status);
        if (event->connect.status == 0) {
            s_ble_connected = true;
            harness_conn_handle = event->connect.conn_handle;
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "ble: disconnect reason=%d", event->disconnect.reason);
        harness_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_ble_connected = false;
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "ble: adv complete reason=%d", event->adv_complete.reason);
        s_ble_adv_active = false;
        return 0;
    default:
        return 0;
    }
}

static const char *BLE_DEVICE_NAME = "OneCollar-Bringup";

static void bringup_ble_start_adv(void)
{
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "ble: infer_auto rc=%d", rc); return; }
    struct ble_hs_adv_fields fields = {0};
    fields.flags            = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name             = (uint8_t *)BLE_DEVICE_NAME;
    fields.name_len         = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "ble: adv_set_fields rc=%d", rc); return; }

    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p,
                           bringup_gap_event_handler, NULL);
    if (rc == 0) {
        s_ble_adv_active = true;
        ESP_LOGI(TAG, "ble: advertising as \"%s\"", BLE_DEVICE_NAME);
    } else {
        ESP_LOGE(TAG, "ble: adv_start rc=%d", rc);
    }
}

static void bringup_ble_on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) == 0) bringup_ble_start_adv();
}

static void bringup_ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "ble: stack reset reason=%d", reason);
}

static void bringup_ble_host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static check_result_t check_ble(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        report("ble", CHECK_FAIL, "nimble_port_init -> %s", esp_err_to_name(err));
        return CHECK_FAIL;
    }
    ble_hs_cfg.reset_cb = bringup_ble_on_reset;
    ble_hs_cfg.sync_cb  = bringup_ble_on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    /* Harness GATT service. Failure is non-fatal per the firmware failure
     * policy — advertising still comes up, the walk still completes. */
    if (bringup_ble_harness_register() != 0) {
        ESP_LOGW(TAG, "harness: registration failed; advertising will continue without it");
    }
    nimble_port_freertos_init(bringup_ble_host_task);

    /* 10 s for advertising to come up — connection testing belongs in the REPL
     * (and in fact in the phone app), not in the walk. */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10 * 1000);
    while (xTaskGetTickCount() < deadline && !s_ble_adv_active) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_ble_adv_active) {
        report("ble", CHECK_PASS, "advertising as \"%s\"", BLE_DEVICE_NAME);
        return CHECK_PASS;
    }
    report("ble", CHECK_FAIL, "advertising never started within 10 s");
    return CHECK_FAIL;
}

static void log_banner(void)
{
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " OneCollar bring-up (walk + REPL)");
    ESP_LOGI(TAG, "   board:   %s (HW rev %d)", BOARD_NAME, BOARD_HW_REV);
    ESP_LOGI(TAG, "   target:  ESP32-S3");
    ESP_LOGI(TAG, "   IDF:     %s", esp_get_idf_version());
    ESP_LOGI(TAG, "================================================");
}

static void walk_all(void)
{
    log_banner();
    check_nvs();
    check_i2c_bus();
    check_i2c_scan();
    check_imu_whoami();
    check_fuel();
    check_charger_en();
    check_lora_version();
    check_cc1101_id();
    check_gps();
    check_ble();

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, " walk summary: %d PASS, %d FAIL, %d SKIP",
             s_pass_count, s_fail_count, s_skip_count);
    ESP_LOGI(TAG, "================================================");
    if (s_fail_count == 0) {
        ESP_LOGI(TAG, "Walk clean. Drop into REPL for hands-on poking.");
    } else {
        ESP_LOGW(TAG, "%d subsystem(s) failed; investigate via REPL before "
                      "starting feature work.", s_fail_count);
    }
}

// ===========================================================================
// REPL — line reader and command dispatcher
// ===========================================================================

/* Strip leading/trailing whitespace, collapse internal whitespace runs to a
 * single space, and upper-case everything. Mirrors the .ino's grammar so a
 * bench operator with .ino muscle memory can paste commands unchanged. */
static void normalize(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);

    size_t L = strlen(s);
    while (L > 0 && (s[L-1] == '\r' || s[L-1] == '\n' ||
                     s[L-1] == ' '  || s[L-1] == '\t')) {
        s[--L] = '\0';
    }

    char *r = s, *w = s;
    bool in_ws = false;
    while (*r) {
        if (*r == ' ' || *r == '\t') {
            if (!in_ws) { *w++ = ' '; in_ws = true; }
        } else {
            *w++ = (char)toupper((unsigned char)*r);
            in_ws = false;
        }
        r++;
    }
    *w = '\0';
}

static void dispatch(char *cmd)
{
    normalize(cmd);
    if (cmd[0] == '\0') return;
    printf("\n>> %s\n", cmd);

    if (!strcmp(cmd, "HELP") || !strcmp(cmd, "?"))                       { cmd_help();         return; }
    if (!strcmp(cmd, "STATUS"))                                          { cmd_status();       return; }
    if (!strcmp(cmd, "PINS"))                                            { cmd_pins();         return; }
    if (!strcmp(cmd, "I2C") || !strcmp(cmd, "SCAN"))                     { cmd_i2c_scan();     return; }
    if (!strcmp(cmd, "I2C LEVELS"))                                      { cmd_i2c_levels();   return; }
    if (!strcmp(cmd, "I2C RECOVER"))                                     { cmd_i2c_recover();  return; }
    if (!strcmp(cmd, "I2C FIND"))                                        { cmd_i2c_find();     return; }
    if (!strcmp(cmd, "I2C PINS NORMAL"))                                 { cmd_i2c_pins_normal(); return; }
    if (!strcmp(cmd, "I2C PINS SWAP") || !strcmp(cmd, "I2C PINS SWAPPED")) { cmd_i2c_pins_swap(); return; }
    if (!strcmp(cmd, "I2C 100"))                                         { cmd_i2c_speed(100000); return; }
    if (!strcmp(cmd, "I2C 400"))                                         { cmd_i2c_speed(400000); return; }
    if (!strcmp(cmd, "FUEL") || !strcmp(cmd, "BAT"))                     { cmd_fuel();          return; }
    if (!strcmp(cmd, "IMU INIT"))                                        { cmd_imu_init();      return; }
    if (!strcmp(cmd, "IMU READ"))                                        { cmd_imu_read_once(); return; }
    if (!strcmp(cmd, "IMU STREAM ON"))                                   { cmd_imu_stream_on(); return; }
    if (!strcmp(cmd, "IMU STREAM OFF"))                                  { cmd_imu_stream_off();return; }
    if (!strncmp(cmd, "GPS READ", 8)) {
        uint32_t ms = 3000;
        const char *space = strchr(cmd + 8, ' ');
        if (space) {
            long v = strtol(space + 1, NULL, 10);
            if (v >= 100 && v <= 60000) ms = (uint32_t)v;
        }
        cmd_gps_read(ms);
        return;
    }
    if (!strcmp(cmd, "GPS RESET"))                                       { cmd_gps_reset();         return; }
    if (!strcmp(cmd, "GPS SAFEBOOT ON"))                                 { cmd_gps_safeboot(true);  return; }
    if (!strcmp(cmd, "GPS SAFEBOOT OFF"))                                { cmd_gps_safeboot(false); return; }
    if (!strcmp(cmd, "LORA VER") || !strcmp(cmd, "LORA ID"))             { cmd_lora_version();      return; }
    if (!strcmp(cmd, "LORA RESET"))                                      { cmd_lora_reset();        return; }
    if (!strcmp(cmd, "CC1101 ID")    || !strcmp(cmd, "CC ID"))           { cmd_cc1101_id();         return; }
    if (!strcmp(cmd, "CC1101 RESET") || !strcmp(cmd, "CC RESET"))        { cmd_cc1101_reset();      return; }
    if (!strcmp(cmd, "CHG EN")  || !strcmp(cmd, "CHARGER EN"))           { cmd_chg(true);           return; }
    if (!strcmp(cmd, "CHG DIS") || !strcmp(cmd, "CHARGER DIS"))          { cmd_chg(false);          return; }

    printf("Unknown command. Type HELP.\n");
}

/* CITE: USB-Serial-JTAG console setup pattern.
 *   esp-idf/examples/system/console/advanced/main/console_settings.c
 *   docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/usb-serial-jtag-console.html
 *
 * Without this dance, the default VFS leaves stdin in a non-blocking mode where
 * fgets() returns immediately with an empty buffer. We need:
 *   1. Set RX line-endings to CR — idf_monitor sends '\r' on Enter
 *   2. Set TX line-endings to CRLF for terminal sanity
 *   3. fcntl(O_BLOCKING) on stdin/stdout
 *   4. Install the interrupt-driven USB-Serial-JTAG driver
 *   5. Point VFS at the new driver
 *   6. Disable stdin buffering so single bytes flow through to our reader
 */
static void repl_console_init(void)
{
    fflush(stdout);
    fsync(fileno(stdout));

    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

    fcntl(fileno(stdout), F_SETFL, 0);
    fcntl(fileno(stdin),  F_SETFL, 0);

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "usb_serial_jtag_driver_install -> %s", esp_err_to_name(err));
    }
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, NULL, _IONBF, 0);
}

static void repl_run(void)
{
    char line[160];
    while (1) {
        printf("\nOneCollar> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* Transient host disconnect or EOF — back off and retry. */
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        dispatch(line);
    }
}

// ===========================================================================
// app_main
// ===========================================================================
void app_main(void)
{
    walk_all();
    repl_console_init();
    cmd_help();
    repl_run();
}
