# OneCollar Hardware Reference

Loaded via `@./.claude/hardware.md`. Factual reference for both Rev 6
(shipping) and Rev 7 (in design). **`board.h` is authoritative for pin
numbers** — this doc is for context, not hardcoded values.

## Multi-rev repo structure (target)

One firmware repo, multiple hardware revs, mirroring `OneCollarHardware/`'s
pattern. No `OneCollarFirmwareV3` ever.

```
OneCollarFirmwareV2/
├── boards/
│   ├── rev6/
│   │   ├── board.h          # Rev 6 pin map (LSM6DSO32X)
│   │   ├── sdkconfig.defaults
│   │   └── partitions.csv
│   └── rev7/
│       ├── board.h          # Rev 7 pin map (ICM-45685 + I2S mic)
│       ├── sdkconfig.defaults
│       └── partitions.csv
├── components/              # Shared drivers, board-agnostic
│   ├── i2c_bus/
│   ├── imu_lsm6dso32x/      # Rev 6 only
│   ├── imu_icm45685/        # Rev 7 only
│   ├── fuel_max17048/
│   ├── ble_service/
│   └── ...
├── main/                    # App entry, board-agnostic above HAL
├── CMakeLists.txt
└── sdkconfig.defaults       # Common defaults; per-board overrides under boards/
```

Build per-board: `idf.py -D BOARD=rev6 build` or `BOARD=rev7`.

## Rev 6 (shipping today, boards arriving for bring-up)

- **MCU**: ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB PSRAM, 240 MHz dual-core)
- **IMU**: ST LSM6DSO32X (6-axis, ±32 g / ±2000 dps, on-chip MLC machine
  learning core for activity wake)
- **Fuel gauge**: Maxim MAX17048
- **Charger**: TI BQ25185 (Rev 6 fixed the JLCPCB DFM footprint issue from Rev 5)
- **LDO**: AP2112K-3.3
- **LoRa** (populated): HopeRF RFM95W
- **GPS** (populated, external breakout): u-blox NEO-M8Q via 6-pin JST-SH
- **Barometer** (DNP): Bosch BMP390 — re-evaluate if altitude features earn
  their power cost
- **Sub-GHz radio** (DNP, dropping for Rev 7): TI CC1101

### Rev 6 I2C bus (shared, 400 kHz, 4.7 kΩ pull-ups on PCB)

| Device | Address | Notes |
|--------|---------|-------|
| LSM6DSO32X IMU | 0x6A (SA0=GND) | Verify on bring-up; can be 0x6B if SA0=VDD |
| MAX17048 fuel gauge | 0x36 | Fixed |
| BMP390 baro (DNP) | 0x77 | Only present after hand-solder |

### Rev 6 I2C scan expected output (bring-up validation)

Without barometer hand-soldered: `0x36, 0x6A`
With barometer: `0x36, 0x6A, 0x77`

### Rev 6 IMU IDs (WHO_AM_I validation)

- LSM6DSO32X `WHO_AM_I` register (0x0F) returns `0x6C`

## Rev 7 (in design — TBDs flagged)

Carry-over from Rev 6 unless noted.

- **IMU**: TDK ICM-45685 SmartMotion (replaces LSM6DSO32X) — on-chip Sensor
  Inference Framework supports richer Tier 0 classifiers than LSM6DSO32X MLC
  decision trees. **NEW Sep 2025; training data thin; reference Arduino
  driver `tdk-invn-oss/motion.arduino.ICM45686` for ICM-456xx register map.**
- **Microphone (NEW)**: I2S MEMS, candidates SPH0645LM4H or ICS-43434.
  Acoustic vent (Gore or equivalent) required behind mic in enclosure.
- **VAD (evaluating)**: Infineon IM73A135 PDM + on-chip VAD for always-on
  audio wake without MCU duty. Bench test pending.
- **Sub-GHz**: CC1101 **proposed for removal** pending firmware
  dependency check.
- **Battery**: LiPo, target 500 mAh, finalization pending enclosure decisions.

### Rev 7 I2C bus (planned)

| Device | Address | Notes |
|--------|---------|-------|
| ICM-45685 IMU | TBD | Verify against datasheet on first sample; family is typically 0x68 or 0x69 (AD0 pin selectable) |
| MAX17048 fuel gauge | 0x36 | Carry-over |
| BMP390 baro (DNP) | 0x77 | Carry-over |

The ICM-45685 address being different from LSM6DSO32X means board.h diff
between revs, not just driver swap.

## Power budget (Rev 7 target, per `02_onecollar_technical_architecture.md` §11)

24-hour average current: **< 5 mA** (stretch < 2 mA in home-dominant use).

| Sink | Average | Notes |
|------|---------|-------|
| Tier 0 IMU always-on | ~15 µA | ICM-45685 low-power classifier mode |
| Tier 1 inference | ~15 µA | 200 events/day × 8 mA × 80 ms |
| Tier 2 inference | ~2 µA | 30 events/day × 25 mA × 200 ms |
| BLE advertising | ~200 µA | 1 Hz interval |
| Wi-Fi sync bursts | ~50 µA | 2 min/day at home |
| LoRa beacon | ~30 µA | 1 beacon/hour when lost |
| **GPS (home-gated)** | **~2 mA** | ~1 hour/day active fixing |
| **GPS (away-heavy)** | **~10+ mA** | Continuous 1 Hz fixes — opt-in mode |

**GPS duty cycle is the dominant variable.** Every other optimization is a
rounding error relative to it. Do not propose GPS-on-always architectures
without explicit user opt-in.

## GPS gating policy (firmware logic, not just config)

- **At home** (Wi-Fi SSID present OR BLE hub in range) → GPS off, opportunistic BLE/Wi-Fi sync
- **Outside home** (geofence exit detected) → GPS on (duty cycled), LoRa as backup
- **Lost** (no known Wi-Fi, no BLE, no GPS fix for N minutes) → LoRa distress beacon

## Physical / mechanical (Rev 7 targets)

- Board: ~80×30 mm, 4-layer (Rev 6 baseline; review for Rev 7)
- Enclosure: IP67 target, acoustic vent behind mic, GPS-permissive top surface
- Mount: standard D-ring collar attachment

## Hardware collaborator

Patrick Carberry (Upwork) — schematic and layout. Adam owns silicon
selection and firmware. KiCad 9 + SkiDL toolchain in `OneCollarHardware/`.
