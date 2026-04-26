# OneCollar Firmware

ESP-IDF firmware for the OneCollar smart dog collar, supporting multiple
hardware revisions in a single tree.

**Status:** scaffold. The structure is in place; driver bodies are stubs.
This compiles into something that links and boots, but does not yet talk to
real hardware — that's the next phase.

## Supported revisions

| Rev | Module | IMU | Mic | Sub-GHz | Status |
|-----|--------|-----|-----|---------|--------|
| 6   | ESP32-S3-WROOM-1-N16R8 | LSM6DSO32X | — | CC1101 | shipping |
| 7   | ESP32-S3-WROOM-1-N16R8 | LSM6DSV320X | I2S MEMS | removed | in design |

Both revs are ST LSM6 family; the IMU port is genuinely a swap, not a
re-architecture. Differences that matter at the firmware layer are the
SFLP block, the dedicated high-g channel, and audio — all rev-specific
capabilities exposed through their own HAL ifaces.

## Build

ESP-IDF v5.x. Set up your environment per the IDF docs, then:

```bash
# Default — Rev 6
idf.py build
idf.py -p PORT flash monitor

# Rev 7
idf.py -DBOARD=rev7 build
idf.py -p PORT flash monitor
```

Switching revs requires a clean build (`idf.py fullclean`) because the
sdkconfig overlay differs.

## Layout

```
onecollar-firmware/
├── CMakeLists.txt              project root, validates BOARD variable
├── partitions.csv              16 MB layout: 2× OTA, models, sessions
├── sdkconfig.defaults          shared ESP-IDF config
├── sdkconfig.defaults.rev6     rev-specific overlay
├── sdkconfig.defaults.rev7
│
├── boards/
│   ├── rev6/
│   │   ├── board.h             pin map + BOARD_HAS_* capability flags
│   │   └── board_config.cmake  driver source selection
│   └── rev7/
│       ├── board.h
│       └── board_config.cmake
│
├── main/
│   ├── CMakeLists.txt          per-rev source assembly
│   ├── main.c                  rev-agnostic boot sequence
│   ├── i2c_bus.{h,c}           shared I2C wrapper
│   ├── data_logger.{h,c}       capability-tagged session logger
│   ├── hal/
│   │   ├── imu_iface.h         common: low-g + gyro + MLC
│   │   ├── sflp_iface.h        Rev 7 only: chip-native gravity/quat
│   │   ├── highg_iface.h       Rev 7 only: ±320 g impact channel
│   │   └── audio_iface.h       Rev 7 only: I2S MEMS mic
│   └── drivers/
│       ├── imu_lsm6dso32x.c    Rev 6 IMU backend
│       ├── imu_lsm6dsv320x.c   Rev 7 IMU backend (also SFLP + high-g)
│       ├── audio_i2s_mems.c    Rev 7 audio backend
│       ├── sflp_iface_stub.c   Rev 6 fallback (returns NOT_SUPPORTED)
│       ├── highg_iface_stub.c  Rev 6 fallback
│       ├── audio_iface_stub.c  Rev 6 fallback
│       └── ... (fuel, ble, lora, gps, power — all rev-shared)
│
└── models/
    ├── rev6/                   Tier 0 UCF + Tier 1 TFLite (Unico-GUI)
    └── rev7/                   Tier 0 UCF + Tier 1 + Tier 2 (MEMS Studio)
```

## Architectural patterns

### `board.h` is the only file that names GPIOs

Every driver includes `board.h` and uses `BOARD_*` defines. No GPIO numbers
appear elsewhere. To port to a new PCB pinout, edit one file.

### Capability flags drive code paths, not preprocessor mazes

Each `board.h` is the **single source of truth** for `BOARD_HAS_IMU_SFLP`,
`BOARD_HAS_AUDIO`, and the rest of the capability bits. Drivers and `main.c`
branch on these at the C level. `main/CMakeLists.txt` parses `board.h` at
configure time (`parse_board_capabilities()`) and surfaces the same names as
CMake variables, so the build system uses identical predicates to decide
whether to compile the real driver or the capability stub. There is no
parallel declaration of these flags in CMake — the two sides cannot drift.

### HAL ifaces split rev-common from rev-specific

`imu_iface.h` is genuinely common between revs (both LSM6 family, same MLC
capacity, same UCF format). SFLP, high-g, and audio are rev-specific
capabilities and live in their own iface headers — calling `sflp_read()` on
Rev 6 returns `ESP_ERR_NOT_SUPPORTED` cleanly, no compile-time branching
needed at the call site.

### Sessions are capability-tagged

`data_logger.c` writes a header per session recording which capabilities
were actually present at capture time. The cloud-side training pipeline
reads these flags to build training jobs that correctly handle the
heterogeneous data lake — Rev 7 audio models only see Rev 7 sessions; gravity
features can train against both with appropriate provenance tracking. This
is the firmware-layer mechanism that preserves Option B (multi-task with
pose supervision) optionality across rev transitions.

## Adding a new hardware revision

1. `boards/revN/board.h` — copy the closest existing board, update GPIO
   assignments and `BOARD_HAS_*` flags. This is the only place capability
   flags need to be declared; CMake parses them out of this file.
2. `boards/revN/board_config.cmake` — set `BOARD_IMU_DRIVER` and
   `BOARD_TIER0_TOOLCHAIN`, and add the `ONECOLLAR_BOARD_REVN=1` compile
   define. Do not redeclare `BOARD_HAS_*` here.
3. If the new rev has a different IMU, add `main/drivers/imu_<part>.c`
   implementing `imu_iface.h` (and optionally `sflp_iface.h` /
   `highg_iface.h` if applicable).
4. Add `revN` to the `VALID_BOARDS` list in root `CMakeLists.txt`.
5. Add `models/revN/` for the new rev's model bundles.
6. Add `sdkconfig.defaults.revN` (can be empty if no overrides needed).

That's it. No fork. No `OneCollarFirmwareV3`.

## Status of stubs

Everything in `main/drivers/` is currently a skeleton — function signatures
implemented per the iface, bodies log and return `ESP_OK`. The remaining
work, in rough priority order:

1. `i2c_bus.c` — real ESP-IDF i2c_master integration with mutex.
2. `imu_lsm6dso32x.c` — Rev 6 bring-up. Vendor `lsm6dso32x-pid` from ST as
   a component. WHO_AM_I check, ODR config, FIFO drain, MLC INT1 wiring.
3. `data_logger.c` — LittleFS mount on `sessions` partition; session header
   write; framed append; BLE-driven upload.
4. `ble_service.c` — NimBLE host + NUS GATT, clean-sheet packet protocol
   matching what the Flutter app team is authoring.
5. `imu_lsm6dsv320x.c` — Rev 7 bring-up. Awaits eval board (STEVAL-MKI251A)
   plus first Rev 7 PCB from Patrick.
6. `audio_i2s_mems.c`, `lora_rfm95w.c`, `gps_neo_m8q.c`, `power.c` — fill
   out as their respective subsystems come online.

## References

- Project root: `02_onecollar_technical_architecture.md` (v0.1.2 with the
  LSM6DSV320X correction)
- Project root: `08_kickoff_brief.md`
- ST drivers: <https://github.com/STMicroelectronics> — search for
  `lsm6dso32x-pid` and `lsm6dsv320x-pid`. BSD-licensed, drop-in.

## License

Proprietary — OneCollar. (Match attribution to the existing V2 `README.md`
line when consolidating into the repo proper.)
