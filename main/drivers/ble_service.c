/**
 * @file ble_service.c
 * @brief Production BLE service — custom GATT service per the platform contract.
 *
 * Implements ../onecollar-platform/contracts/ble_protocol.md (envelope, GATT
 * layout, handshake) with payloads from contracts/ble_protocol.proto via the
 * generated nanopb bindings in main/ble/. The GATT registration, envelope
 * codec, LESC/bonding configuration, and time-sync handler graduate from the
 * register sequences verified on Rev 6 silicon by the bring-up harness
 * (bringup_rev6.c — Android eval pass, decision log 2026-06-09).
 *
 * Request coverage policy: every ToCollar variant gets a contract-conformant
 * response. Handlers whose backing driver is not implemented yet return
 * STATUS_NOT_READY (retryable; firmware work pending) — never silence, never
 * fake data. Hardware the board genuinely lacks returns STATUS_UNSUPPORTED.
 *
 * Threading: every handler runs on the NimBLE host task. Large protobuf
 * unions live in static storage, not on the 4 KB host-task stack; the
 * single-threaded dispatch makes that safe.
 */

#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "pb_decode.h"
#include "pb_encode.h"
#include "ble_protocol.pb.h"

#include "freertos/ringbuf.h"

#include "board.h"
#include "data_logger.h"
#include "hal/imu_iface.h"
#include "imu_sampler.h"
#include "drivers/ble_service.h"
#include "drivers/fuel.h"

static const char *TAG = "oc.ble";

/* Schema version advertised in the capability handshake
 * (contract §Versioning). Proto package is onecollar.ble.v1. */
#define BLE_SCHEMA_MAJOR             1
#define BLE_SCHEMA_MINOR             0
#define BLE_MIN_APP_SUPPORTED_MAJOR  1

/* Production advertising name. The contract does not yet specify a naming
 * convention (drift-audit row 20 is open) — when it does, this constant
 * follows the contract. The mobile scanner filters by name because the adv
 * packet carries name + flags only (no room for the 128-bit service UUID). */
static const char *BLE_DEVICE_NAME = "OneCollar";

// ---------------------------------------------------------------------------
// GATT layout — contract §GATT layout. UUIDs are little-endian in
// BLE_UUID128_INIT (bytes reversed from the human-readable form).
//   Service:   0cbe0000-1000-4001-a000-000000000001
//   cmd_rx:    0cbe0001-...   (write, write-no-rsp, encrypted)
//   cmd_tx:    0cbe0002-...   (notify)
//   event_tx:  0cbe0003-...   (notify)
//   stream_tx: 0cbe0004-...   (notify)
// ---------------------------------------------------------------------------
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0,
    0x01, 0x40, 0x00, 0x10, 0x00, 0x00, 0xbe, 0x0c);
static const ble_uuid128_t cmd_rx_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0,
    0x01, 0x40, 0x00, 0x10, 0x01, 0x00, 0xbe, 0x0c);
static const ble_uuid128_t cmd_tx_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0,
    0x01, 0x40, 0x00, 0x10, 0x02, 0x00, 0xbe, 0x0c);
static const ble_uuid128_t event_tx_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0,
    0x01, 0x40, 0x00, 0x10, 0x03, 0x00, 0xbe, 0x0c);
static const ble_uuid128_t stream_tx_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0,
    0x01, 0x40, 0x00, 0x10, 0x04, 0x00, 0xbe, 0x0c);

static uint16_t s_cmd_tx_handle;
static uint16_t s_event_tx_handle;
static uint16_t s_stream_tx_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

// IMU streaming (Stage B): a dedicated task drains the imu_sampler ring buffer
// into ImuBatch frames on stream_tx. ImuBatch.samples caps at 480 B = 40 samples
// and fits one MTU-512 fragment, so each batch is a single notify.
#define IMU_STREAM_BATCH_MAX   40     // samples/batch (ImuBatch.samples cap)
#define IMU_STREAM_PERIOD_MS   250    // pump cadence (~25 samples/batch at ~100 Hz)
static volatile bool s_streaming = false;
static TaskHandle_t  s_stream_task = NULL;
static uint16_t      s_stream_seq = 0;
static uint32_t      s_stream_rate_hz = 0;       // measured (not nominal) — see start handler
static uint32_t      s_stream_batches_sent = 0;
static uint32_t      s_stream_batches_dropped = 0;

// IMU capture-to-flash (Stage C): a task drains the same imu_sampler ring into
// batched frames written to the session file via data_logger. Runs autonomously
// — it survives a BLE disconnect (off-leash capture is the point). Mutually
// exclusive with streaming (one ring consumer at a time).
#define IMU_CAPTURE_BATCH_MAX  120    // samples/flash frame (120*12+14 = 1454 B)
#define IMU_CAPTURE_PERIOD_MS  250
static volatile bool s_capturing = false;
static TaskHandle_t  s_capture_task = NULL;
static uint32_t      s_capture_rate_hz = 0;
static uint32_t      s_capture_frames = 0;
static uint32_t      s_capture_samples = 0;
static void capture_log_task(void *arg);  // defined after the shared drain helper

/* Wire-format counters, served verbatim by GetDebugStats. */
static onecollar_ble_v1_DebugStats s_stats;

// ---------------------------------------------------------------------------
// Envelope — contract §Envelope.
//   [length:2 LE][type:1][flags:1][txn_or_seq:2 LE][payload:N][crc16:2 LE]
// CRC-16-CCITT-FALSE over header + payload (not the CRC itself).
// ---------------------------------------------------------------------------
#define ENV_TYPE_TO_COLLAR    0x01
#define ENV_TYPE_FROM_COLLAR  0x02
#define ENV_TYPE_EVENT        0x03
#define ENV_TYPE_IMU_BATCH    0x04
#define ENV_FLAG_MORE_FRAGMENTS 0x01
#define ENV_HEADER_SIZE       6
#define ENV_TRAILER_SIZE      2
#define ENV_OVERHEAD          (ENV_HEADER_SIZE + ENV_TRAILER_SIZE)

/* Largest single ATT write we accept: MTU 512 − ATT header 3, rounded up. */
#define BLE_FRAME_MAX 600

static uint16_t crc16_ccitt_false(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

typedef struct {
    uint8_t        type;
    uint8_t        flags;
    uint16_t       txn_or_seq;
    const uint8_t *payload;
    uint16_t       payload_len;
} env_frame_t;

/* Returns 0 on success; negative on malformed/short/CRC-bad. */
static int env_decode(const uint8_t *buf, uint16_t buf_len, env_frame_t *out)
{
    if (buf_len < ENV_OVERHEAD) return -1;
    uint16_t plen = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    if ((size_t)plen + ENV_OVERHEAD != buf_len) return -2;
    uint16_t crc_recv = (uint16_t)buf[buf_len - 2] | ((uint16_t)buf[buf_len - 1] << 8);
    uint16_t crc_calc = crc16_ccitt_false(buf, buf_len - 2);
    if (crc_recv != crc_calc) return -3;
    out->type        = buf[2];
    out->flags       = buf[3];
    out->txn_or_seq  = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
    out->payload     = buf + ENV_HEADER_SIZE;
    out->payload_len = plen;
    return 0;
}

/* Returns total bytes written, or negative on overflow. */
static int env_encode(uint8_t type, uint8_t flags, uint16_t txn_or_seq,
                      const uint8_t *payload, uint16_t payload_len,
                      uint8_t *out_buf, size_t out_buf_size)
{
    if (out_buf_size < (size_t)payload_len + ENV_OVERHEAD) return -1;
    out_buf[0] = (uint8_t)(payload_len & 0xFF);
    out_buf[1] = (uint8_t)((payload_len >> 8) & 0xFF);
    out_buf[2] = type;
    out_buf[3] = flags;
    out_buf[4] = (uint8_t)(txn_or_seq & 0xFF);
    out_buf[5] = (uint8_t)((txn_or_seq >> 8) & 0xFF);
    memcpy(out_buf + ENV_HEADER_SIZE, payload, payload_len);
    uint16_t crc = crc16_ccitt_false(out_buf, ENV_HEADER_SIZE + payload_len);
    out_buf[ENV_HEADER_SIZE + payload_len]     = (uint8_t)(crc & 0xFF);
    out_buf[ENV_HEADER_SIZE + payload_len + 1] = (uint8_t)((crc >> 8) & 0xFF);
    return ENV_HEADER_SIZE + payload_len + ENV_TRAILER_SIZE;
}

// ---------------------------------------------------------------------------
// Send path. A FromCollar that exceeds one MTU (Capabilities can, at the
// nanopb worst case) is split across envelope frames sharing the txn_id,
// MORE_FRAGMENTS set on all but the last (contract §Fragmentation).
// ---------------------------------------------------------------------------
static int ble_notify_frame(uint16_t chr_handle, const uint8_t *frame, int frame_len)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, frame_len);
    if (om == NULL) {
        ESP_LOGE(TAG, "mbuf alloc failed (%d bytes)", frame_len);
        return BLE_HS_ENOMEM;
    }
    int rc = ble_gatts_notify_custom(s_conn_handle, chr_handle, om);
    if (rc == 0) {
        s_stats.bytes_sent += (uint64_t)frame_len;
    } else {
        ESP_LOGW(TAG, "notify rc=%d", rc);
    }
    return rc;
}

static int ble_send_payload(uint8_t env_type, uint16_t chr_handle,
                            uint16_t txn_or_seq,
                            const uint8_t *payload, size_t payload_len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return BLE_HS_ENOTCONN;

    uint16_t mtu = ble_att_mtu(s_conn_handle);
    if (mtu < ENV_OVERHEAD + 4 + 3) mtu = BLE_ATT_MTU_DFLT;
    size_t max_frag = (size_t)(mtu - 3) - ENV_OVERHEAD;  /* ATT header is 3 B */

    static uint8_t frame[BLE_FRAME_MAX];
    size_t off = 0;
    do {
        size_t chunk = payload_len - off;
        bool more = chunk > max_frag;
        if (more) chunk = max_frag;
        int frame_len = env_encode(env_type,
                                   more ? ENV_FLAG_MORE_FRAGMENTS : 0,
                                   txn_or_seq,
                                   payload + off, (uint16_t)chunk,
                                   frame, sizeof(frame));
        if (frame_len < 0) return frame_len;
        int rc = ble_notify_frame(chr_handle, frame, frame_len);
        if (rc != 0) return rc;
        off += chunk;
    } while (off < payload_len);
    return 0;
}

/* Encode + send one FromCollar on cmd_tx. `resp` is static in the caller. */
static void ble_send_from_collar(uint16_t txn_id,
                                 const onecollar_ble_v1_FromCollar *resp)
{
    /* FromCollar's worst case is ~5.4 KB (Capabilities at nanopb bounds) —
     * far beyond the 4 KB host-task stack, so the encode buffer is static.
     * Single-threaded dispatch on the host task makes that safe. */
    static uint8_t pb_buf[onecollar_ble_v1_FromCollar_size];
    pb_ostream_t os = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    if (!pb_encode(&os, onecollar_ble_v1_FromCollar_fields, resp)) {
        ESP_LOGE(TAG, "FromCollar pb_encode failed: %s", PB_GET_ERROR(&os));
        return;
    }
    if (ble_send_payload(ENV_TYPE_FROM_COLLAR, s_cmd_tx_handle,
                         txn_id, pb_buf, os.bytes_written) == 0) {
        s_stats.responses_sent++;
    }
}

/* Shorthand for the many requests that reply with status only. */
static void ble_send_status(uint16_t txn_id, onecollar_ble_v1_Status status)
{
    static onecollar_ble_v1_FromCollar resp;
    memset(&resp, 0, sizeof(resp));
    resp.status = status;
    ble_send_from_collar(txn_id, &resp);
}

// ---------------------------------------------------------------------------
// Request handlers. Each fills the shared static FromCollar and sends it.
// ---------------------------------------------------------------------------
static onecollar_ble_v1_FromCollar s_resp;  /* host-task only; see file header */

static void handle_time_sync(uint16_t txn_id,
                             const onecollar_ble_v1_TimeSyncRequest *req)
{
    /* t2 immediately on entry; t3 immediately before encode, as close to the
     * wire send as we can get (contract §Time-sync handshake). */
    uint64_t t2 = (uint64_t)esp_timer_get_time();
    memset(&s_resp, 0, sizeof(s_resp));
    s_resp.status                            = onecollar_ble_v1_Status_STATUS_OK;
    s_resp.which_response                    = onecollar_ble_v1_FromCollar_time_sync_tag;
    s_resp.response.time_sync.phone_send_us  = req->phone_send_us;
    s_resp.response.time_sync.collar_recv_us = t2;
    s_resp.response.time_sync.collar_send_us = (uint64_t)esp_timer_get_time();
    ble_send_from_collar(txn_id, &s_resp);
}

static void cap_add(onecollar_ble_v1_Capabilities *caps,
                    const char *id, bool available)
{
    if (caps->capabilities_count >= 32) return;
    onecollar_ble_v1_Capability *c = &caps->capabilities[caps->capabilities_count++];
    strncpy(c->id, id, sizeof(c->id) - 1);
    c->state = available ? onecollar_ble_v1_CapabilityState_CAPABILITY_AVAILABLE
                         : onecollar_ble_v1_CapabilityState_CAPABILITY_UNAVAILABLE;
    c->unlock_hint[0] = '\0';  /* no entitlement gating yet */
}

static void handle_get_capabilities(uint16_t txn_id)
{
    memset(&s_resp, 0, sizeof(s_resp));
    s_resp.status         = onecollar_ble_v1_Status_STATUS_OK;
    s_resp.which_response = onecollar_ble_v1_FromCollar_capabilities_tag;
    onecollar_ble_v1_Capabilities *caps = &s_resp.response.capabilities;
    caps->schema_major            = BLE_SCHEMA_MAJOR;
    caps->schema_minor            = BLE_SCHEMA_MINOR;
    caps->min_app_supported_major = BLE_MIN_APP_SUPPORTED_MAJOR;
    caps->hw_revision             = BOARD_HW_REV;
    cap_add(caps, "imu.lowg",        BOARD_HAS_IMU_LOWG);
    cap_add(caps, "imu.highg",       BOARD_HAS_IMU_HIGHG);
    cap_add(caps, "imu.sflp",        BOARD_HAS_IMU_SFLP);
    cap_add(caps, "imu.mlc",         BOARD_HAS_IMU_MLC);
    cap_add(caps, "audio.tier2",     BOARD_HAS_AUDIO);
    cap_add(caps, "gps",             BOARD_HAS_GPS);
    cap_add(caps, "lora.relay",      BOARD_HAS_LORA);
    cap_add(caps, "capture.session", true);
    ble_send_from_collar(txn_id, &s_resp);
}

static void handle_get_system_status(uint16_t txn_id)
{
    memset(&s_resp, 0, sizeof(s_resp));
    s_resp.status         = onecollar_ble_v1_Status_STATUS_OK;
    s_resp.which_response = onecollar_ble_v1_FromCollar_system_status_tag;
    onecollar_ble_v1_SystemStatus *st = &s_resp.response.system_status;
    const esp_app_desc_t *app = esp_app_get_description();
    strncpy(st->firmware_version, app->version, sizeof(st->firmware_version) - 1);
    st->hw_revision      = BOARD_HW_REV;
    st->boot_time_us     = 0;  /* collar clock epoch IS boot (esp_timer) */
    st->capability_flags = data_logger_current_capabilities();
    ble_send_from_collar(txn_id, &s_resp);
}

static void handle_get_system_stats(uint16_t txn_id)
{
    memset(&s_resp, 0, sizeof(s_resp));
    s_resp.status         = onecollar_ble_v1_Status_STATUS_OK;
    s_resp.which_response = onecollar_ble_v1_FromCollar_system_stats_tag;
    onecollar_ble_v1_SystemStats *st = &s_resp.response.system_stats;
    st->task_count       = (uint32_t)uxTaskGetNumberOfTasks();
    st->free_heap_kb     = (uint32_t)(esp_get_free_heap_size() / 1024);
    st->min_free_heap_kb = (uint32_t)(esp_get_minimum_free_heap_size() / 1024);
    st->uptime_minutes   = (uint32_t)(esp_timer_get_time() / 60000000ULL);
    /* cpu_pct needs CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS and temperature_c
     * needs the on-die sensor driver — both report 0 until wired, not fake
     * values. error_count is reserved for the (future) error registry. */
    st->cpu_pct       = 0;
    st->error_count   = 0;
    st->temperature_c = 0;
    ble_send_from_collar(txn_id, &s_resp);
}

static void handle_get_mtu_status(uint16_t txn_id)
{
    memset(&s_resp, 0, sizeof(s_resp));
    s_resp.status         = onecollar_ble_v1_Status_STATUS_OK;
    s_resp.which_response = onecollar_ble_v1_FromCollar_mtu_status_tag;
    uint16_t mtu = ble_att_mtu(s_conn_handle);
    s_resp.response.mtu_status.negotiated_mtu   = mtu;
    s_resp.response.mtu_status.max_payload_size = (uint32_t)(mtu - 3);
    ble_send_from_collar(txn_id, &s_resp);
}

static void handle_get_battery(uint16_t txn_id)
{
    fuel_status_t fs;
    if (fuel_read(&fs) != ESP_OK) {
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_NOT_READY);
        return;
    }
    memset(&s_resp, 0, sizeof(s_resp));
    s_resp.status         = onecollar_ble_v1_Status_STATUS_OK;
    s_resp.which_response = onecollar_ble_v1_FromCollar_battery_tag;
    onecollar_ble_v1_BatteryStatus *b = &s_resp.response.battery;
    b->percentage   = fs.soc_percent;
    b->voltage_mv   = fs.voltage_mv;
    b->alert_active = fs.alert_active;
    b->alert_code   = 0;  /* no firmware alert codes defined yet */
    switch (fs.charge) {
    case FUEL_CHARGE_CHARGING:    b->charge = onecollar_ble_v1_ChargeState_CHARGE_CHARGING;    break;
    case FUEL_CHARGE_DISCHARGING: b->charge = onecollar_ble_v1_ChargeState_CHARGE_DISCHARGING; break;
    case FUEL_CHARGE_FULL:        b->charge = onecollar_ble_v1_ChargeState_CHARGE_FULL;        break;
    case FUEL_CHARGE_CRITICAL:    b->charge = onecollar_ble_v1_ChargeState_CHARGE_CRITICAL;    break;
    default:                      b->charge = onecollar_ble_v1_ChargeState_CHARGE_UNKNOWN;     break;
    }
    ble_send_from_collar(txn_id, &s_resp);
}

static void handle_get_imu_snapshot(uint16_t txn_id)
{
    imu_sample_t sample;
    esp_err_t err = imu_read_sample(&sample);
    if (err != ESP_OK) {
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_NOT_READY);
        return;
    }
    memset(&s_resp, 0, sizeof(s_resp));
    s_resp.status         = onecollar_ble_v1_Status_STATUS_OK;
    s_resp.which_response = onecollar_ble_v1_FromCollar_imu_snapshot_tag;
    onecollar_ble_v1_ImuSnapshot *snap = &s_resp.response.imu_snapshot;
    snap->timestamp_us = sample.timestamp_us;
    snap->accel_x_mg   = sample.accel_x_mg;
    snap->accel_y_mg   = sample.accel_y_mg;
    snap->accel_z_mg   = sample.accel_z_mg;
    snap->gyro_x_mdps  = sample.gyro_x_mdps;
    snap->gyro_y_mdps  = sample.gyro_y_mdps;
    snap->gyro_z_mdps  = sample.gyro_z_mdps;
    ble_send_from_collar(txn_id, &s_resp);
}

static void handle_set_wall_clock(uint16_t txn_id,
                                  const onecollar_ble_v1_SetWallClockRequest *req)
{
    /* The system clock IS the wall clock — settimeofday so every consumer
     * (data_logger session headers, future log timestamps) reads it through
     * gettimeofday with no cross-module plumbing. */
    struct timeval tv = {
        .tv_sec  = (time_t)(req->unix_us / 1000000ULL),
        .tv_usec = (suseconds_t)(req->unix_us % 1000000ULL),
    };
    if (settimeofday(&tv, NULL) != 0) {
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_INTERNAL);
        return;
    }
    ESP_LOGI(TAG, "wall clock set: unix_us=%llu", (unsigned long long)req->unix_us);
    ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_OK);
}

static void handle_set_debug_level(uint16_t txn_id,
                                   const onecollar_ble_v1_SetDebugLevelRequest *req)
{
    static const esp_log_level_t levels[] = {
        ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN,
        ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE,
    };
    if (req->level > 5) {
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_INVALID_ARG);
        return;
    }
    esp_log_level_set("*", levels[req->level]);
    ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_OK);
}

static void handle_set_config(uint16_t txn_id,
                              const onecollar_ble_v1_SetConfigRequest *req)
{
    if (req->key[0] == '\0') {
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_INVALID_ARG);
        return;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("config", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, req->key, req->value.bytes, req->value.size);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    ble_send_status(txn_id, err == ESP_OK
                                ? onecollar_ble_v1_Status_STATUS_OK
                                : onecollar_ble_v1_Status_STATUS_INTERNAL);
}

static void reset_timer_cb(void *arg)
{
    esp_restart();
}

static void handle_reset(uint16_t txn_id, const onecollar_ble_v1_ResetRequest *req)
{
    /* ACK first, then reset after delay_ms (contract). One-shot esp_timer so
     * the host task isn't blocked while the ACK drains. */
    ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_OK);
    static esp_timer_handle_t reset_timer;
    const esp_timer_create_args_t args = {
        .callback = reset_timer_cb,
        .name     = "oc.ble.reset",
    };
    uint64_t delay_us = (uint64_t)req->delay_ms * 1000ULL;
    if (delay_us < 50000ULL) delay_us = 50000ULL;  /* let the ACK leave first */
    if (reset_timer == NULL && esp_timer_create(&args, &reset_timer) != ESP_OK) {
        esp_restart();
    }
    esp_timer_start_once(reset_timer, delay_us);
}

static void handle_start_capture(uint16_t txn_id,
                                 const onecollar_ble_v1_StartCaptureRequest *req)
{
    if (req->rate_hz != 0 && req->rate_hz != 20 && req->rate_hz != 100) {
        /* Rate tiers are locked at {20, 100} (contract §Locked decisions). */
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_INVALID_ARG);
        return;
    }
    if (s_streaming) {  // one ring consumer at a time (capture XOR stream)
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_BUSY);
        return;
    }
    datalog_session_header_t hdr;
    esp_err_t err = data_logger_session_open(&hdr);
    if (err == ESP_ERR_INVALID_STATE) {
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_BUSY);
        return;
    }
    if (err != ESP_OK) {
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_INTERNAL);
        return;
    }
    // Measured rate (the DSO32X "104 Hz" runs ~98.5 Hz; logged frames carry it
    // so the session reconstructs sample times honestly — same as the stream).
    imu_sampler_stats_t st;
    imu_sampler_get_stats(&st);
    if (st.samples > 1 && st.last_us > st.start_us) {
        uint64_t span = st.last_us - st.start_us;
        s_capture_rate_hz = (uint32_t)(((st.samples - 1) * 1000000ULL + span / 2) / span);
    } else {
        s_capture_rate_hz = (hdr.imu_odr_hz ? hdr.imu_odr_hz : 104);
    }
    s_capture_frames = 0;
    s_capture_samples = 0;
    s_capturing = true;
    if (xTaskCreatePinnedToCore(capture_log_task, "imu_capture", 4096, NULL, 5,
                                &s_capture_task, 0) != pdPASS) {
        s_capturing = false;
        data_logger_session_close();
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_INTERNAL);
        return;
    }
    memset(&s_resp, 0, sizeof(s_resp));
    s_resp.status         = onecollar_ble_v1_Status_STATUS_OK;
    s_resp.which_response = onecollar_ble_v1_FromCollar_start_capture_tag;
    s_resp.response.start_capture.session_id    = hdr.session_id;
    s_resp.response.start_capture.start_time_us = (uint64_t)esp_timer_get_time();
    ble_send_from_collar(txn_id, &s_resp);
}

static void handle_stop_capture(uint16_t txn_id)
{
    // Stop the logger and JOIN it before closing, so no append races the close.
    s_capturing = false;
    for (int i = 0; i < 50 && s_capture_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    esp_err_t err = data_logger_session_close();
    ble_send_status(txn_id, err == ESP_OK
                                ? onecollar_ble_v1_Status_STATUS_OK
                                : onecollar_ble_v1_Status_STATUS_NOT_READY);
}

static void handle_label_tag(uint16_t txn_id,
                             const onecollar_ble_v1_LabelTagRequest *req)
{
    /* Collar logs the label as DATALOG_STREAM_USER_LABEL at collar-clock
     * receive time; STATUS_NOT_READY if no capture is active (contract). */
    size_t len = strnlen(req->label, sizeof(req->label));
    if (len == 0) {
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_INVALID_ARG);
        return;
    }
    esp_err_t err = data_logger_append(DATALOG_STREAM_USER_LABEL, req->label, len);
    ble_send_status(txn_id, err == ESP_OK
                                ? onecollar_ble_v1_Status_STATUS_OK
                                : onecollar_ble_v1_Status_STATUS_NOT_READY);
}

// ---------------------------------------------------------------------------
// IMU streaming (Stage B). The stream task owns its own encode buffers (NOT the
// host-task s_resp / ble_send_payload statics) so it never races them; it calls
// ble_notify_frame directly (NimBLE serializes notify internally). An ImuBatch
// is <= 480 B payload -> one MTU-512 fragment, so a single env_encode + notify.
// ---------------------------------------------------------------------------
bool ble_service_is_streaming(void) { return s_streaming; }
bool ble_service_is_capturing(void) { return s_capturing; }

/* Drain up to `max` raw samples from the sampler ring, packing each as
 * (ax,ay,az,gx,gy,gz) LE int16 (12 B) into `out`. Returns the count; *base_ts is
 * the first sample's collar timestamp. Shared by the BLE stream and the flash
 * logger — only one of them is ever an active ring consumer at a time. */
static int imu_drain_packed(uint8_t *out, int max, uint64_t *base_ts)
{
    RingbufHandle_t rb = imu_sampler_ringbuf();
    if (!rb) return 0;
    int n = 0;
    while (n < max) {
        size_t sz = 0;
        imu_raw_sample_t *s = (imu_raw_sample_t *)xRingbufferReceive(rb, &sz, 0);
        if (s == NULL) break;
        if (sz == sizeof(*s)) {
            if (n == 0) *base_ts = s->timestamp_us;
            const int16_t v[6] = { s->accel[0], s->accel[1], s->accel[2],
                                   s->gyro[0],  s->gyro[1],  s->gyro[2] };
            for (int k = 0; k < 6; k++) {
                out[n * 12 + k * 2]     = (uint8_t)(v[k] & 0xFF);
                out[n * 12 + k * 2 + 1] = (uint8_t)(((uint16_t)v[k] >> 8) & 0xFF);
            }
            n++;
        }
        vRingbufferReturnItem(rb, s);
    }
    return n;
}

/* Drain one ImuBatch worth of samples and notify it on stream_tx. Stream-task-
 * only statics. Returns true if a batch was sent, false if empty or send failed. */
static bool stream_send_one_batch(void)
{
    static onecollar_ble_v1_ImuBatch batch;   // stream-task-only
    uint64_t base_ts = 0;
    int n = imu_drain_packed(batch.samples.bytes, IMU_STREAM_BATCH_MAX, &base_ts);
    if (n == 0) return false;

    batch.seq               = s_stream_seq;
    batch.base_timestamp_us = base_ts;
    batch.rate_hz           = s_stream_rate_hz;
    batch.sample_count      = (uint32_t)n;
    batch.samples.size      = (pb_size_t)(n * 12);

    static uint8_t pb_buf[onecollar_ble_v1_ImuBatch_size];
    pb_ostream_t os = pb_ostream_from_buffer(pb_buf, sizeof(pb_buf));
    if (!pb_encode(&os, onecollar_ble_v1_ImuBatch_fields, &batch)) {
        ESP_LOGE(TAG, "ImuBatch encode failed: %s", PB_GET_ERROR(&os));
        return false;
    }
    static uint8_t frame[BLE_FRAME_MAX];
    int flen = env_encode(ENV_TYPE_IMU_BATCH, 0, s_stream_seq,
                          pb_buf, (uint16_t)os.bytes_written, frame, sizeof(frame));
    if (flen < 0) return false;
    int rc = ble_notify_frame(s_stream_tx_handle, frame, flen);
    if (rc != 0) {
        s_stream_batches_dropped++;
        if (rc == BLE_HS_ENOTCONN) s_streaming = false;  // peer gone — stop
        return false;
    }
    s_stream_seq++;
    s_stream_batches_sent++;
    return true;
}

static void imu_stream_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "imu stream task started (rate=%u Hz)", s_stream_rate_hz);
    while (s_streaming) {
        while (s_streaming && stream_send_one_batch()) { /* clear any backlog */ }
        vTaskDelay(pdMS_TO_TICKS(IMU_STREAM_PERIOD_MS));
    }
    ESP_LOGI(TAG, "imu stream task stopped (sent=%u dropped=%u)",
             s_stream_batches_sent, s_stream_batches_dropped);
    s_stream_task = NULL;
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// IMU capture-to-flash (Stage C). Same ring, same packed-int16 payload, but the
// batch is written to the session file (data_logger) instead of notified. On-
// flash IMU frame payload (DATALOG_STREAM_IMU_LOWG):
//   [base_timestamp_us:8 LE][rate_hz:4 LE][sample_count:2 LE][N x 12B samples]
// i.e. a persisted ImuBatch — maps 1:1 onto a contract SessionChunk on read-back.
// ---------------------------------------------------------------------------
static bool capture_log_one_batch(void)
{
    static uint8_t frame[14 + IMU_CAPTURE_BATCH_MAX * 12];  // capture-task-only
    uint64_t base_ts = 0;
    int n = imu_drain_packed(frame + 14, IMU_CAPTURE_BATCH_MAX, &base_ts);
    if (n == 0) return false;

    for (int i = 0; i < 8; i++) frame[i] = (uint8_t)((base_ts >> (8 * i)) & 0xFF);
    uint32_t r = s_capture_rate_hz;
    frame[8]  = (uint8_t)(r & 0xFF);
    frame[9]  = (uint8_t)((r >> 8) & 0xFF);
    frame[10] = (uint8_t)((r >> 16) & 0xFF);
    frame[11] = (uint8_t)((r >> 24) & 0xFF);
    frame[12] = (uint8_t)(n & 0xFF);
    frame[13] = (uint8_t)((n >> 8) & 0xFF);

    esp_err_t e = data_logger_append(DATALOG_STREAM_IMU_LOWG, frame,
                                     (size_t)(14 + n * 12));
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "capture append failed (%s) — stopping logger",
                 esp_err_to_name(e));
        s_capturing = false;   // flash full / FS error: stop cleanly
        return false;
    }
    s_capture_frames++;
    s_capture_samples += (uint32_t)n;
    return true;
}

static void capture_log_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "capture log task started (rate=%u Hz)", s_capture_rate_hz);
    while (s_capturing) {
        while (s_capturing && capture_log_one_batch()) { /* clear backlog */ }
        vTaskDelay(pdMS_TO_TICKS(IMU_CAPTURE_PERIOD_MS));
    }
    ESP_LOGI(TAG, "capture log task stopped (frames=%u samples=%u)",
             s_capture_frames, s_capture_samples);
    s_capture_task = NULL;
    vTaskDelete(NULL);
}

static void handle_start_imu_stream(uint16_t txn_id,
                                    const onecollar_ble_v1_StartImuStreamRequest *req)
{
    (void)req;  // rate is collar-clamped; we stream the sampler's native ODR
    if (s_capturing) {  // one ring consumer at a time (stream XOR capture)
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_BUSY);
        return;
    }
    if (s_streaming) {  // idempotent
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_OK);
        return;
    }
    // Report the MEASURED rate (the DSO32X "104 Hz" runs ~98.5 Hz on this unit —
    // see docs/10 bring-up log). The consumer reconstructs sample times from
    // base_timestamp_us + N/rate_hz, so the measured rate (not nominal) keeps
    // intra-batch timing honest; each batch re-bases off its first real sample.
    imu_sampler_stats_t st;
    imu_sampler_get_stats(&st);
    if (st.samples > 1 && st.last_us > st.start_us) {
        uint64_t span = st.last_us - st.start_us;
        s_stream_rate_hz = (uint32_t)(((st.samples - 1) * 1000000ULL + span / 2) / span);
    } else {
        imu_info_t info;
        s_stream_rate_hz = (imu_get_info(&info) == ESP_OK && info.odr_hz) ? info.odr_hz : 104;
    }
    s_stream_seq = 0;
    s_stream_batches_sent = 0;
    s_stream_batches_dropped = 0;
    s_streaming = true;
    if (xTaskCreatePinnedToCore(imu_stream_task, "imu_stream", 4096, NULL, 6,
                                &s_stream_task, 0) != pdPASS) {
        s_streaming = false;
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_INTERNAL);
        return;
    }
    ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_OK);
}

static void handle_stop_imu_stream(uint16_t txn_id)
{
    s_streaming = false;  // the task observes this and exits
    ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_OK);
}

static void handle_get_stream_status(uint16_t txn_id)
{
    memset(&s_resp, 0, sizeof(s_resp));
    s_resp.status         = onecollar_ble_v1_Status_STATUS_OK;
    s_resp.which_response = onecollar_ble_v1_FromCollar_stream_status_tag;
    onecollar_ble_v1_StreamStatus *st = &s_resp.response.stream_status;
    st->running         = s_streaming;
    st->rate_hz         = s_stream_rate_hz;
    st->batches_sent    = s_stream_batches_sent;
    st->samples_buffered = 0;   // consumer-drained ring; not separately tracked
    st->buffer_capacity = 256;
    st->batches_dropped = s_stream_batches_dropped;
    ble_send_from_collar(txn_id, &s_resp);
}

static void handle_echo(uint16_t txn_id, const onecollar_ble_v1_EchoRequest *req)
{
    memset(&s_resp, 0, sizeof(s_resp));
    s_resp.status         = onecollar_ble_v1_Status_STATUS_OK;
    s_resp.which_response = onecollar_ble_v1_FromCollar_echo_tag;
    s_resp.response.echo.payload.size = req->payload.size;
    memcpy(s_resp.response.echo.payload.bytes, req->payload.bytes, req->payload.size);
    ble_send_from_collar(txn_id, &s_resp);
}

static void handle_test_pattern(uint16_t txn_id,
                                const onecollar_ble_v1_TestPatternRequest *req)
{
    size_t cap = sizeof(s_resp.response.test_pattern.payload.bytes);
    if (req->length > cap) {
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_INVALID_ARG);
        return;
    }
    memset(&s_resp, 0, sizeof(s_resp));
    s_resp.status         = onecollar_ble_v1_Status_STATUS_OK;
    s_resp.which_response = onecollar_ble_v1_FromCollar_test_pattern_tag;
    s_resp.response.test_pattern.pattern_id   = req->pattern_id;
    s_resp.response.test_pattern.payload.size = (pb_size_t)req->length;
    for (uint32_t i = 0; i < req->length; i++) {
        s_resp.response.test_pattern.payload.bytes[i] =
            (uint8_t)(req->pattern_id + i);
    }
    ble_send_from_collar(txn_id, &s_resp);
}

static void handle_get_debug_stats(uint16_t txn_id)
{
    memset(&s_resp, 0, sizeof(s_resp));
    s_resp.status               = onecollar_ble_v1_Status_STATUS_OK;
    s_resp.which_response       = onecollar_ble_v1_FromCollar_debug_stats_tag;
    s_resp.response.debug_stats = s_stats;
    ble_send_from_collar(txn_id, &s_resp);
}

// ---------------------------------------------------------------------------
// Dispatch — one ToCollar in, exactly one contract-conformant reply out.
// ---------------------------------------------------------------------------
static void dispatch_to_collar(uint16_t txn_id, const onecollar_ble_v1_ToCollar *to)
{
    s_stats.commands_received++;
    switch (to->which_request) {
    case onecollar_ble_v1_ToCollar_time_sync_tag:
        handle_time_sync(txn_id, &to->request.time_sync);
        break;
    case onecollar_ble_v1_ToCollar_get_capabilities_tag:
        handle_get_capabilities(txn_id);
        break;
    case onecollar_ble_v1_ToCollar_get_system_status_tag:
        handle_get_system_status(txn_id);
        break;
    case onecollar_ble_v1_ToCollar_get_system_stats_tag:
        handle_get_system_stats(txn_id);
        break;
    case onecollar_ble_v1_ToCollar_get_mtu_status_tag:
        handle_get_mtu_status(txn_id);
        break;
    case onecollar_ble_v1_ToCollar_get_battery_tag:
        handle_get_battery(txn_id);
        break;
    case onecollar_ble_v1_ToCollar_get_imu_snapshot_tag:
        handle_get_imu_snapshot(txn_id);
        break;
    case onecollar_ble_v1_ToCollar_set_wall_clock_tag:
        handle_set_wall_clock(txn_id, &to->request.set_wall_clock);
        break;
    case onecollar_ble_v1_ToCollar_set_debug_level_tag:
        handle_set_debug_level(txn_id, &to->request.set_debug_level);
        break;
    case onecollar_ble_v1_ToCollar_set_config_tag:
        handle_set_config(txn_id, &to->request.set_config);
        break;
    case onecollar_ble_v1_ToCollar_reset_tag:
        handle_reset(txn_id, &to->request.reset);
        break;
    case onecollar_ble_v1_ToCollar_start_capture_tag:
        handle_start_capture(txn_id, &to->request.start_capture);
        break;
    case onecollar_ble_v1_ToCollar_stop_capture_tag:
        handle_stop_capture(txn_id);
        break;
    case onecollar_ble_v1_ToCollar_label_tag_tag:
        handle_label_tag(txn_id, &to->request.label_tag);
        break;
    case onecollar_ble_v1_ToCollar_echo_tag:
        handle_echo(txn_id, &to->request.echo);
        break;
    case onecollar_ble_v1_ToCollar_test_pattern_tag:
        handle_test_pattern(txn_id, &to->request.test_pattern);
        break;
    case onecollar_ble_v1_ToCollar_get_debug_stats_tag:
        handle_get_debug_stats(txn_id);
        break;

    /* GPS / LoRa / streaming / session list+read+delete / geofence:
     * backing implementations pending (see data_logger.c TODOs for the
     * session surface). All retryable, hence NOT_READY. */
    case onecollar_ble_v1_ToCollar_start_imu_stream_tag:
        handle_start_imu_stream(txn_id, &to->request.start_imu_stream);
        break;
    case onecollar_ble_v1_ToCollar_stop_imu_stream_tag:
        handle_stop_imu_stream(txn_id);
        break;
    case onecollar_ble_v1_ToCollar_get_stream_status_tag:
        handle_get_stream_status(txn_id);
        break;

    case onecollar_ble_v1_ToCollar_get_gps_tag:
    case onecollar_ble_v1_ToCollar_get_lora_status_tag:
    case onecollar_ble_v1_ToCollar_get_all_radios_tag:
    case onecollar_ble_v1_ToCollar_get_imu_batch_tag:  /* time-range query needs buffered history (Stage C) */
    case onecollar_ble_v1_ToCollar_list_sessions_tag:
    case onecollar_ble_v1_ToCollar_read_session_tag:
    case onecollar_ble_v1_ToCollar_delete_session_tag:
    case onecollar_ble_v1_ToCollar_set_geofence_tag:
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_NOT_READY);
        break;

    case onecollar_ble_v1_ToCollar_get_rf_status_tag:
#if BOARD_HAS_SUBGHZ_RADIO
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_NOT_READY);
#else
        /* CC1101 removed in Rev 7 — genuinely absent, not pending. */
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_UNSUPPORTED);
#endif
        break;

    default:
        /* Newer app, older firmware: graceful-degrade (contract §Versioning). */
        ble_send_status(txn_id, onecollar_ble_v1_Status_STATUS_UNSUPPORTED);
        break;
    }
}

// ---------------------------------------------------------------------------
// cmd_rx write callback — envelope decode → ToCollar decode → dispatch.
// ---------------------------------------------------------------------------
static int ble_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0;  /* TX characteristics are notify-only */
    }
    static uint8_t buf[BLE_FRAME_MAX];
    uint16_t out_len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &out_len);
    if (rc != 0) {
        ESP_LOGW(TAG, "cmd_rx flatten rc=%d", rc);
        return rc;
    }
    s_stats.bytes_received += (uint64_t)out_len;

    env_frame_t frame;
    int env_rc = env_decode(buf, out_len, &frame);
    if (env_rc == -3) {
        s_stats.crc_errors++;
        return 0;  /* envelope layer drops on CRC mismatch; no retry */
    }
    if (env_rc != 0 || frame.type != ENV_TYPE_TO_COLLAR) {
        s_stats.frame_errors++;
        return 0;
    }
    if (frame.flags & ENV_FLAG_MORE_FRAGMENTS) {
        /* Every v1 ToCollar fits one MTU (contract §Fragmentation); inbound
         * reassembly lands with the reliability layer (DECISION NEEDED). */
        ESP_LOGW(TAG, "fragmented ToCollar not supported; dropping");
        s_stats.frame_errors++;
        return 0;
    }

    static onecollar_ble_v1_ToCollar to;
    memset(&to, 0, sizeof(to));
    pb_istream_t is = pb_istream_from_buffer(frame.payload, frame.payload_len);
    if (!pb_decode(&is, onecollar_ble_v1_ToCollar_fields, &to)) {
        ESP_LOGW(TAG, "ToCollar pb_decode failed: %s", PB_GET_ERROR(&is));
        ble_send_status(frame.txn_or_seq,
                        onecollar_ble_v1_Status_STATUS_INVALID_ARG);
        return 0;
    }
    dispatch_to_collar(frame.txn_or_seq, &to);
    return 0;
}

// ---------------------------------------------------------------------------
// GATT table
// ---------------------------------------------------------------------------
static const struct ble_gatt_chr_def ble_chrs[] = {
    {
        .uuid = &cmd_rx_uuid.u,
        .access_cb = ble_chr_access,
        /* WRITE_ENC forces a pair on first connect; bonded centrals reuse
         * the stored LTK afterward (LESC + NVS persistence, verified by
         * eval gate 4 on Rev 6 silicon). */
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP
               | BLE_GATT_CHR_F_WRITE_ENC,
    },
    {
        .uuid = &cmd_tx_uuid.u,
        .access_cb = ble_chr_access,
        .val_handle = &s_cmd_tx_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY,
    },
    {
        .uuid = &event_tx_uuid.u,
        .access_cb = ble_chr_access,
        .val_handle = &s_event_tx_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY,
    },
    {
        .uuid = &stream_tx_uuid.u,
        .access_cb = ble_chr_access,
        .val_handle = &s_stream_tx_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY,
    },
    { 0 },
};

static const struct ble_gatt_svc_def ble_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = ble_chrs,
    },
    { 0 },
};

/* val_handles are assigned at host sync (ble_gatts_start), not at
 * add_svcs — this callback is where the real numbers surface. */
static void ble_gatts_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_REGISTER_OP_CHR) return;
    const ble_uuid_t *u = ctxt->chr.chr_def->uuid;
    const char *name = NULL;
    if      (ble_uuid_cmp(u, &cmd_rx_uuid.u)    == 0) name = "cmd_rx";
    else if (ble_uuid_cmp(u, &cmd_tx_uuid.u)    == 0) name = "cmd_tx";
    else if (ble_uuid_cmp(u, &event_tx_uuid.u)  == 0) name = "event_tx";
    else if (ble_uuid_cmp(u, &stream_tx_uuid.u) == 0) name = "stream_tx";
    if (name == NULL) return;
    ESP_LOGI(TAG, "chr %-9s val_handle=0x%04x", name, ctxt->chr.val_handle);
}

// ---------------------------------------------------------------------------
// GAP — advertising + connection lifecycle
// ---------------------------------------------------------------------------
static void ble_start_adv(void);

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connect %s status=%d",
                 event->connect.status == 0 ? "ESTABLISHED" : "FAILED",
                 event->connect.status);
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
        } else {
            ble_start_adv();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_streaming = false;  // stop the stream task if a capture was running
        /* NimBLE stops advertising on connect; re-arm so the bonded central
         * can always come back (no silent drop after first connection). */
        ble_start_adv();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "adv complete reason=%d", event->adv_complete.reason);
        return 0;
    default:
        return 0;
    }
}

static void ble_start_adv(void)
{
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer_auto rc=%d", rc); return; }
    struct ble_hs_adv_fields fields = {0};
    fields.flags            = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name             = (uint8_t *)BLE_DEVICE_NAME;
    fields.name_len         = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }

    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p,
                           ble_gap_event_handler, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "advertising as \"%s\"", BLE_DEVICE_NAME);
    } else {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
    }
}

static void ble_on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) == 0) ble_start_adv();
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "stack reset reason=%d", reason);
}

static void ble_host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
esp_err_t ble_service_init(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init -> %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.reset_cb          = ble_on_reset;
    ble_hs_cfg.sync_cb           = ble_on_sync;
    ble_hs_cfg.gatts_register_cb = ble_gatts_register_cb;
    /* Security Manager runtime flags — NOT covered by the Kconfig options.
     * Without sm_bonding=1 here, pairing succeeds once but never persists
     * (HCI 0x13 on every reconnect — exactly what eval gate 4 hit). NO_IO
     * means Just Works: the collar has no display or keyboard. Both sides
     * distribute LTK + IRK so the bond survives address rotation. */
    ble_hs_cfg.sm_io_cap         = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 0;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);

    int rc = ble_gatts_count_cfg(ble_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(ble_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT registration rc=%d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "BLE service up: schema v%d.%d, 4 chars, LESC+bonding",
             BLE_SCHEMA_MAJOR, BLE_SCHEMA_MINOR);
    return ESP_OK;
}
