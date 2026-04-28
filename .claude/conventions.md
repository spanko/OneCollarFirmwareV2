# OneCollar Firmware Conventions

Loaded via `@./.claude/conventions.md`. Concrete rules for code style,
RTOS patterns, communication framing, and failure policies.

## ESP-IDF version and APIs

- **Required**: ESP-IDF v5.5+
- **Preferred**: v6.0+ (enables `idf.py mcp-server`, multi-config builds,
  `idf.py refresh-config`)
- **Use**: new I2C master driver (`driver/i2c_master.h`), NimBLE host
  (not Bluedroid), new I2S driver (`driver/i2s_std.h`), new GPIO API,
  ESP-NN for quantized inference acceleration.
- **Do NOT use**: legacy I2C driver (`driver/i2c.h`), legacy I2S driver
  (`driver/i2s.h`), Bluedroid host, deprecated `gpio_set_intr_type` patterns.

When uncertain about which API is current, query the Espressif Documentation
MCP server before answering. Do not rely on training-data API recall.

## Project structure conventions

- **`board.h` is the single source of truth for GPIOs.** No pin numbers
  hardcoded in driver `.c` or `.h` files — always reference board macros.
- Component-per-subsystem under `components/` with own `CMakeLists.txt` +
  `Kconfig` + `include/` + `.c` files. No flat source directories.
- Per-board overrides under `boards/rev6/` and `boards/rev7/`.
- Common config in root `sdkconfig.defaults`; board-specific deltas only
  in `boards/<rev>/sdkconfig.defaults`.
- NVS namespaces dedicated per subsystem (`config`, `geofence`, `behavior`,
  `adapt_head`) — no shared namespace dumping ground.
- Compiled test executables and `monitor_*.py` scripts do NOT belong in
  the repo root (V1 had this; do not propagate). Tests under `test/`.

## FreeRTOS task plan

| Task | Core | Priority | Stack | Cadence |
|------|------|----------|-------|---------|
| IMU sample / Tier 0 dispatch | 0 | 8 | 4 KB | Tier 0 wake-driven |
| Tier 1 inference | 1 | 6 | 8 KB | Tier 0 events |
| Tier 2 inference (audio) | 1 | 6 | 8 KB | Tier 1 escalation |
| BLE service | 0 | 5 | 4 KB | NimBLE host events |
| Fuel gauge poll | 0 | 2 | 2 KB | 60 s |
| GPS (when policy enables) | 0 | 4 | 4 KB | Duty-cycled |
| LoRa | 0 | 4 | 4 KB | Beacon-driven |

ML inference on Core 1, sensing/comms on Core 0. Watchdog: every task
adds itself with `esp_task_wdt_add(NULL)` after creation.

## Failure policy

Asymmetric, intentional:

- **IMU init failure** → fatal. Log, then halt with `esp_restart()` after
  N retries (do NOT use the V2 scaffold's `while(1) vTaskDelay(1000ms)`
  pattern — that bricks the device from a user perspective with no
  recovery path).
- **Fuel gauge / charger / baro / GPS / LoRa init failure** → non-fatal,
  logged warning, subsystem disabled, boot continues.
- **NVS truncation or version mismatch** → auto-erase and reinitialize
  with defaults, log warning.

## DNP-aware boot

Optional peripherals (`baro_init`, `gps_init`, `lora_init`, `cc1101_init`
on Rev 6) MUST return gracefully on absence and not block boot. Detection
via I2C ACK probe or SPI silicon-rev read at init time, not via compile-time
config alone. Compile-time `BOARD=rev7` selects which peripherals are
*expected*; runtime probe confirms which are *present*.

## BLE protocol

- **GATT service**: Nordic UART Service (NUS), UUIDs preserved from V1
  for app-side compatibility:
  - Service: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
  - RX (app→collar): `6E400002-...`
  - TX (collar→app): `6E400003-...`
- **MTU**: re-negotiate to 512 on every connection. Default 23/20-byte
  payload is insufficient.
- **Packet framing** (clean-sheet for V2; V1's reliability layer ideas as
  inspiration only — do not port C# code from `WearDoggoTrainer/`):
  ```
  [type(1)] [seq(1)] [len_lo(1)] [len_hi(1)] [payload(N)] [checksum(1)]
  ```
  Single-byte XOR checksum. Little-endian length. Payload up to MTU-3.
- Reliability layer (SACK, flow control, fragmentation) to be rebuilt
  fresh against latest ESP-IDF NimBLE. Design first, implement second.

## Sensor sampling

- IMU at 104 Hz Tier 1 baseline, 25-50 Hz Tier 0 always-on. ODR is per-tier,
  not global.
- Audio I2S at 16 kHz mono 16-bit (Rev 7 only).
- Use ringbuffer pattern (ESP-SR style) for IMU and audio — never block
  in sample handlers.

## Model deployment

- Format: TFLite flatbuffer + JSON manifest (class labels, input shape,
  tier assignment, version, base-vs-adaptation role).
- Dedicated model partition in flash (separate from OTA A/B firmware slots).
- Models update independently of firmware revs.
- Cadence target: base models monthly, per-dog adaptation heads weekly.
- On-device fine-tuning is a future capability; for now adaptation heads
  are fine-tuned in cloud and pushed.

## Logging

- Use ESP_LOGI/W/E with per-component tags. Tag format: `"oc.imu"`,
  `"oc.ble"`, `"oc.tier1"` — `oc.` prefix to grep cleanly.
- Boot banner with rev string, board ID (`rev6`/`rev7`), git short SHA,
  ESP-IDF version. Bring-up sketch reads this banner as a sanity check.
- Avoid Unicode box-drawing characters in log output (V2 scaffold's
  `──` separators) — multi-byte UTF-8 noise in serial dumps.

## Capture and labeling (preserves Option B)

- Sessions store **raw** IMU + audio + (phone-side) video at native rates.
- Windowing happens at training time, not capture time.
- Session ID is ms-epoch start time. Per-session metadata in JSON.
- File layout follows the columnar pattern from `LastCollarTestBed`
  (compatible with dataship/beam) so existing notebooks can ingest:
  ```
  /sessions/<ms_epoch>/
    timestamp.u32
    accel_{x,y,z}.f32
    gyro_{x,y,z}.f32
    audio.s16   (Rev 7 only)
    pose_keypoints.json   (cloud-extracted, post-hoc)
    labels.json   (user-provided, multi-source)
    metadata.json
  ```
- Labels are first-class and multi-source: user, pose-derived,
  model-predicted are all retained with provenance.

## Forbidden patterns

- Hardcoded Azure connection strings, API keys, BLE bond keys, or cert
  material in source. Use Key Vault references or env vars.
- Two implementations of the same subsystem compiled together
  (V1's `bluetooth_service.c` + `bluetooth_service_enhanced.c` problem).
  One canonical implementation per subsystem; experiments live on branches.
- Periodic unsolicited BLE broadcasts (V1 had a periodic battery broadcast
  that thrashed the connection). Command-response only by default; explicit
  subscription for streaming.
- TODOs without an issue link. If incomplete, file an issue and reference
  it: `// TODO(#42): implement SACK gap recovery`.
- Hardcoded timezones (V1 testbed hardcoded US Central, no DST). Use
  UTC internally; format for display only.
