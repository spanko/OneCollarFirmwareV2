# OneCollarFirmwareV2 — Project Instructions for Claude Code

This repo is the Rev 7 firmware for the OneCollar smart dog collar platform.
Currently a clean-slate scaffold (3 files, board.h authoritative for pinout).

@./.claude/architecture.md
@./.claude/hardware.md
@./.claude/conventions.md

@./docs/01_product_vision.md
@./docs/02_onecollar_technical_architecture.md
@./docs/08_kickoff_brief.md
@./docs/09_behavior_literature_synthesis.md

## What this firmware is

Continuous-motion-state estimator running on ESP32-S3, feeding a behavior-library
matching layer. NOT a fixed-class behavior classifier. The on-device model
produces a hybrid representation — 6-10 named interpretable features plus a
learned residual embedding — and behavior recognition is a separate library
match step that runs on top.

The platform's defining capability is that users teach new behaviors by
capturing synchronized video, and adding a behavior to the fleet is a library
update, not a model retrain.

## Architectural guardrails (hard rules — do not violate)

1. **Decouple motion-state estimation from behavior recognition.** The model's
   output is a feature vector, not behavior class probabilities. Library
   matching is a separate layer. Never propose architectures that fold these
   back together.

2. **Preserve Option B (pose-supervised multi-task) optionality.** Option A
   (supervised classification from labels) is the current training approach.
   Option B is the aspirational track. Data capture, session schema, and
   any model-shape decisions must keep Option B reachable. If a proposal
   optimizes Option A at the cost of Option B, flag it explicitly and ask.

3. **Novelty detection is a first-class capability**, not a "future feature."
   Design choices must preserve the ability to flag motion states that don't
   match any library entry with confidence.

4. **Raw streams at full fidelity, windowing at training time.** Capture
   sessions store raw IMU, audio, video, and pose at native rates. Never
   pre-window or down-sample at capture.

5. **board.h is the single source of truth for GPIOs.** No hardcoded pin
   numbers in driver code, ever.

## Workflow

- **Build**: `idf.py build` (target ESP32-S3, project esp32s3)
- **Flash + monitor**: `idf.py -p /dev/ttyACM0 flash monitor` (USB native to S3)
- **Clean**: `idf.py fullclean` before any sdkconfig structural change
- **Targets**: Rev 6 (current shipping, LSM6DSO32X) and Rev 7 (in design,
  TDK ICM-45685 + I2S mic). One repo houses both — see hardware.md for
  the multi-rev structure plan.
- **ESP-IDF version**: v5.5+ required, v6.0 preferred (we use idf.py
  mcp-server from v6.0)

## When uncertain about ESP-IDF APIs

Use the Espressif Documentation MCP server before answering. If you find
yourself recalling API signatures from training data, stop and query the
MCP instead. Add "refer to Espressif documentation" to confirm if unsure.

## When uncertain about ICM-45685

The TDK SmartMotion ICM-45685 is new (Sep 2025). Training data is thin.
Reference the tdk-invn-oss/motion.arduino.ICM45686 Arduino driver as the
ground-truth API surface — the ICM-45685 shares the ICM-456xx register
map. Port patterns from there to ESP-IDF I2C/SPI master drivers. Ask
before committing speculative register addresses.

## Bring-up before features

Rev 6 boards are arriving and need validation. The first commits to this
repo should be a bring-up sketch that walks every subsystem and prints
pass/fail to USB serial:
- I2C scan (expect 0x6A IMU, 0x36 fuel gauge, 0x77 baro if hand-soldered)
- IMU WHO_AM_I (LSM6DSO32X = 0x6C; ICM-45685 = 0xE9 on Rev 7)
- MAX17048 voltage/SOC sanity check
- Charger STAT pin read
- RFM95W SPI silicon-rev read (if populated)
- GPS UART NMEA presence (if populated)
- BLE advertise + accept connection

This bring-up code becomes the seed for the real driver implementations.
Don't write features before bring-up passes.

## What NOT to do

- Don't carry forward code from `OneCollarFirmware/` (V1). Lessons yes,
  code no. V1 has two BLE service implementations compiled together,
  three comm_manager variants, renumbered command opcodes. Clean slate.
- Don't carry forward the V1 BLE protocol verbatim. The v2 reliability
  layer (SACK, flow control, fragmentation) ideas are good — rebuild
  them clean against latest ESP-IDF NimBLE, don't port the C# from
  WearDoggoTrainer.
- Don't propose Edge Impulse-only training pipelines. Edge Impulse is
  the near-term unblock; the Azure-native pipeline is the target state.
  Any model export should be re-runnable through both paths.
- Don't auto-fill or reproduce code that suggests proprietary InvenSense
  eMD library internals — those are NDA-gated. Use the public Arduino
  driver as reference, not the eMD source.

## Sibling repos in this product family

- `OneCollarFirmware/` — V1, legacy, snapshot in project context
- `OneCollarHardware/` — KiCad PCB designs (rev 6 shipping, rev 7 in design)
- `WearDoggoTrainer/` — MAUI mobile app, being retired in favor of Flutter
- `WearDoggoTrainerAPI/` — Azure SAS broker (has a known auth gap, see
  kickoff brief, address before broader beta)
- `LastCollarTestBed/` — historical ML/firmware sandbox, snapshot only

## Collaborators

- **Adam Wengert** (you're talking to me) — product, architecture, firmware,
  ML direction
- **Patrick Carberry** — hardware/PCB, engaged via Upwork

## Security rails (always)

- Never commit secrets. Azure connection strings, API keys, vault names
  go in environment variables or Key Vault references, never source.
- Never hardcode test BLE bond keys or production cert material.
- Never paste live Azure storage keys into code or comments. (V1 has
  done this in WearDoggoConsole/Program.cs — do not propagate.)

## When asking me clarifying questions

- Ask at most 3 at a time, prioritized.
- Don't ask about decisions already settled in 08_kickoff_brief.md —
  read that first.