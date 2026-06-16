# OneCollar Hardware Reference

Loaded via `@./.claude/hardware.md`. Factual reference for both Rev 6
(shipping) and Rev 7 (in design). **`board.h` is authoritative for pin
numbers AND capability flags** — this doc is for context, not hardcoded values.

## Multi-rev repo structure (current state)

One firmware repo, multiple hardware revs, mirroring `OneCollarHardware/`'s
pattern. No `OneCollarFirmwareV3` ever.

```
OneCollarFirmwareV2/
├── boards/
│   ├── rev6/
│   │   ├── board.h              # Pin map + BOARD_HAS_* flags (LSM6DSO32X)
│   │   └── board_config.cmake   # Driver source selection
│   └── rev7/
│       ├── board.h              # Pin map + BOARD_HAS_* flags (LSM6DSV320X)
│       └── board_config.cmake
├── main/
│   ├── main.c                   # Rev-agnostic boot
│   ├── i2c_bus.{h,c}            # Shared I2C wrapper
│   ├── data_logger.{h,c}        # Capability-tagged session logger
│   ├── hal/
│   │   ├── imu_iface.h          # Common: low-g + gyro + MLC
│   │   ├── sflp_iface.h         # Rev 7 only: chip-native gravity/quat
│   │   ├── highg_iface.h        # Rev 7 only: ±320 g impact channel
│   │   └── audio_iface.h        # Rev 7 only: I2S MEMS mic
│   ├── drivers/                 # Per-rev backends + capability stubs
│   └── bringup_rev6/            # First-board validation sketch
└── models/
    ├── rev6/                    # Tier 0 UCF + Tier 1 TFLite
    └── rev7/                    # Tier 0 UCF + Tier 1 + Tier 2 audio
```

Build per-board: `idf.py build` (Rev 6 default) or `idf.py -DBOARD=rev7 build`.

## Rev 6 (shipping)

- **MCU:** ESP32-S3-WROOM-1-N16R8 (16 MB flash, 8 MB PSRAM, 240 MHz dual-core)
- **IMU:** ST LSM6DSO32X (6-axis, ±32 g / ±2000 dps, on-chip MLC)
- **Fuel gauge:** Maxim MAX17048 @ 0x36
- **Charger:** TI BQ25185 (Rev 6 fixed the JLCPCB DFM footprint issue from Rev 5)
- **LDO:** AP2112K-3.3
- **LoRa** (populated): HopeRF RFM95W
- **GPS** (populated, external breakout): u-blox NEO-M8Q via 6-pin JST-SH
- **Barometer** (DNP): Bosch BMP390 — re-evaluate if altitude features earn
  their power cost
- **Sub-GHz radio** (DNP, dropped from Rev 7): TI CC1101

### Rev 6 I2C bus (shared, 400 kHz, 4.7 kΩ pull-ups on PCB)

| Device | Address | Notes |
|--------|---------|-------|
| LSM6DSO32X IMU | 0x6A (SA0=GND) | Verify on bring-up |
| MAX17048 fuel gauge | 0x36 | Fixed |
| BMP390 baro (DNP) | 0x77 | Only present after hand-solder |

**I2C scan expected output:** `0x36, 0x6A` (without barometer) or
`0x36, 0x6A, 0x77` (with).

### Rev 6 IMU IDs

- LSM6DSO32X `WHO_AM_I` register (0x0F) returns `0x6C`.

## Rev 7 (in design)

The IMU upgrade is the headline change. Same family as Rev 6 (ST LSM6),
same I2C address (0x6A), same MLC concepts and UCF tooling — so it's a
genuine driver swap, not a toolchain switch. The LSM6DSV320X adds three
new on-chip capabilities the architecture didn't anticipate when Rev 7 was
first scoped:

| Capability | What it does | Architectural implication |
|---|---|---|
| **MLC** (carries over) | 8 trees × 256 nodes of decision-tree inference at sensor ODR | Tier 0 wake-on-activity classifier (same as Rev 6) |
| **FSM** (new) | 8 programmable state machines + ASC | Custom wake patterns beyond MLC trees — e.g. "rest → sudden movement → rest" event triggers without firmware involvement |
| **SFLP** (new) | Chip-native gravity vector + game-rotation quaternion | Gravity-aligned reference frame is now free at sensor level. Tier 1 features that needed firmware-side fusion math get cheaper |
| **High-g channel** (new) | Dedicated ±320 g accelerometer alongside the ±16 g low-g channel | Impact / fall detection becomes a near-free Tier 0 signal on its own data path, no rate trade-off against motion classification |

Carry-overs from Rev 6 unless noted.

- **MCU:** ESP32-S3-WROOM-1-N16R8 (unchanged)
- **IMU:** ST LSM6DSV320X (replaces LSM6DSO32X) — same I2C address 0x6A,
  same INT1/INT2 pin mapping, MLC capacity unchanged
- **Microphone (NEW):** I2S MEMS mic, **part TBD** (PATRICK_TODO on pins).
  Earlier candidates SPH0645LM4H / ICS-43434 are not yet selected. Acoustic
  vent (Gore or equivalent) required behind mic in enclosure.
- **VAD (evaluating):** Infineon IM73A135 PDM + on-chip VAD, default DNP
  (`BOARD_HAS_VAD = 0`). Bench eval pending; revisit whether the LSM6DSV320X's
  MLC-driven wake makes the always-on VAD chip redundant.
- **Sub-GHz:** CC1101 **removed**. GPIO 14 (former CS) freed for reassignment.
- **Battery:** LiPo, target 500 mAh, finalization pending enclosure decisions.

### Rev 7 I2C bus (planned)

| Device | Address | Notes |
|--------|---------|-------|
| LSM6DSV320X IMU | 0x6A (SA0=GND) | Same as Rev 6 — convenient |
| MAX17048 fuel gauge | 0x36 | Carry-over |
| BMP390 baro (DNP) | 0x77 | Carry-over |

### Rev 7 IMU ID

- LSM6DSV320X `WHO_AM_I` value: **TBD — datasheet check before bring-up.**
  Do not guess; mark `// UNVERIFIED` if used in code before the value is
  confirmed.

### Rev 7 PATRICK_TODO items (open before silicon order)

- **LoRa RST GPIO** (currently scratch-assigned)
- **I2S BCLK / WS / DIN GPIO** (mic pin assignment)
- **VAD PDM_CLK / PDM_DAT / INT GPIO** (only if VAD populates)
- Verify INT1 / INT2 routing matches Rev 6 (intent is to keep them stable
  so the IMU HAL wiring is rev-portable)

## Power budget (Rev 7 target, per `../onecollar-platform/docs/02-architecture.md` §11)

24-hour average current: **< 5 mA** (stretch < 2 mA in home-dominant use).

| Sink | Average | Notes |
|------|---------|-------|
| Tier 0 IMU always-on (MLC) | ~15 µA | Wake gate; same order as Rev 6 |
| Tier 0 FSM (Rev 7 new) | TBD — small | State-machine execution at sensor ODR |
| Tier 0 high-g monitor (Rev 7 new) | TBD | Separate accel; need datasheet figure |
| Tier 1 inference | ~15 µA | 200 events/day × 8 mA × 80 ms |
| Tier 2 inference (audio, Rev 7) | ~2 µA | 30 events/day × 25 mA × 200 ms |
| BLE advertising | ~200 µA | 1 Hz interval |
| Wi-Fi sync bursts | ~50 µA | 2 min/day at home |
| LoRa beacon | ~30 µA | 1 beacon/hour when lost |
| **GPS (home-gated)** | **~2 mA** | ~1 hour/day active fixing |
| **GPS (away-heavy)** | **~10+ mA** | Continuous 1 Hz fixes — opt-in mode |

**Two budget refinements the LSM6DSV320X enables:**
- SFLP performs gravity/quat computation at sensor power, not MCU power.
  Tier 1 inference cost may drop modestly because the firmware no longer
  fuses gravity in software.
- High-g and low-g run on parallel paths, so impact detection doesn't cost
  Tier 0 ODR headroom against motion classification.

**GPS duty cycle remains the dominant variable.** Every other optimization
is a rounding error relative to it. Do not propose GPS-on-always
architectures without explicit user opt-in.

## GPS gating policy (firmware logic, not just config)

- **At home** (Wi-Fi SSID present OR BLE hub in range) → GPS off, opportunistic BLE/Wi-Fi sync
- **Outside home** (geofence exit detected) → GPS on (duty cycled), LoRa as backup
- **Lost** (no known Wi-Fi, no BLE, no GPS fix for N minutes) → LoRa distress beacon

## Physical / mechanical (Rev 7 targets)

- Board: ~80×30 mm, 4-layer (Rev 6 baseline; review for Rev 7)
- Enclosure: IP67 target, acoustic vent behind mic, GPS-permissive top surface
- Mount: standard D-ring collar attachment

## Hardware collaborator

Patrick Carberry (Upwork) — schematic and layout, owns PATRICK_TODO items.
Adam owns silicon selection and firmware. **Altium is the authoritative EDA
tool** in `OneCollarHardware/` (KiCad 9 + SkiDL retired 2026-06-15, see platform
decision log).
