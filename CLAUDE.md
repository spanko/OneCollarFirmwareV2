<!-- BEGIN SHARED HEADER · source of truth: onecollar-platform/.claude/templates/repo-CLAUDE.md
     Do NOT edit between these markers. Edit the template, then re-sync every repo.
     spec-auditor flags any drift in this block. -->

# CLAUDE.md — part of the OneCollar platform

*This repo is one implementation of the OneCollar smart dog collar platform. The
**brain** — cross-cutting specs, the locked architectural invariants, cross-repo
contracts, and the decision log — lives in the `onecollar-platform` repo, a
sibling of this one. When a change touches anything cross-cutting, read that repo
first.*

**Before touching anything cross-cutting, read:**
- `../onecollar-platform/CLAUDE.md` — orientation + the five locked invariants.
- `../onecollar-platform/docs/` — the `NN`-series domain specs.
- `../onecollar-platform/contracts/` — the byte-level cross-repo source of truth.
- `../onecollar-platform/decisions/decision-log.md` — the ADR.

(If you opened the `onecollar.code-workspace` multi-root workspace, those are
already in scope. This pointer exists for the case where this repo is opened
alone — a solo clone, a quick fix, or CI — when the workspace is not loaded.)

**The five locked invariants (by reference — owned by the platform repo, never
copied here):** decoupled motion-state / behavior recognition; the behavior
library as a user-facing primitive; novelty detection as first-class; data
collection that preserves optionality; Option A current / Option B preserved.
→ `../onecollar-platform/CLAUDE.md` and `docs/05`, `docs/07`.

**Standing directive:** prefer proposals that preserve Option B optionality and
behavior-library extensibility over solutions that optimize the current approach
at the cost of future flexibility.

**Cross-repo contract rule:** a cross-cutting type (BLE framing, session schema,
library entry, feature set) is born in `../onecollar-platform/contracts/` and
changes there **first** — never invented locally and migrated later.

<!-- END SHARED HEADER -->

# OneCollarFirmwareV2 — Project Instructions for Claude Code

This repo is the OneCollar smart dog collar firmware, supporting multiple
hardware revisions (Rev 6 shipping, Rev 7 in design) in a single tree.

<!-- Cross-cutting docs live in ../onecollar-platform/. The four formerly-
     duplicated docs (01_product_vision, 02_technical_architecture,
     08_kickoff_brief, 09_behavior_literature_synthesis) were deleted on
     2026-06-05; the platform repo is canonical. Origin-story prose was
     preserved at ../onecollar-platform/docs/origin-story.md.

     The three .claude/*.md imports below are KEPT as firmware-side
     distillations + firmware-specific addenda (FreeRTOS task plan,
     BLE frame layout, failure policy). They are not duplicates of
     platform docs; they are the session-context distillation loaded
     every conversation. Future refactor: re-anchor each with an explicit
     "canonical full spec at ../onecollar-platform/docs/NN-*.md" header
     and trim any paraphrase. -->

@./.claude/architecture.md      # firmware distillation; canonical: ../onecollar-platform/docs/02-architecture.md
@./.claude/hardware.md          # firmware-side detail; canonical: ../onecollar-platform/docs/03-hardware.md (currently thinner — lift TODO)
@./.claude/conventions.md       # firmware-local conventions; platform: ../onecollar-platform/docs/00 + docs/01

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

These are the platform's five locked invariants applied to firmware. Canonical
statements live in `../onecollar-platform/CLAUDE.md` and `docs/05`, `docs/07` —
if these ever need to change, change the platform doc first (it is the source of
truth), then re-sync here.

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
5. **Capability-tagged sessions.** Every recorded session header captures
   which `BOARD_HAS_*` flags were true at capture time. Cloud training
   pipelines use this for correct multi-rev mixing.
6. **board.h is the single source of truth for GPIOs and capabilities.**
   No hardcoded pin numbers or capability assumptions in driver code, ever.
   `parse_board_capabilities()` reads board.h at configure time so the C
   side and the build system cannot drift.

## Cross-repo contracts (firmware-relevant)

Anything firmware shares with the phone or cloud is owned by the platform repo's
`contracts/`, and changes there first (see the shared header). The two firmware
touches:
- BLE framing/commands → `../onecollar-platform/contracts/ble_protocol.md`
- Session / data schema → `../onecollar-platform/contracts/data_schema.md`
Do not define these locally and migrate later.

## Workflow

- **Build (Rev 6, default):** `idf.py build`
- **Build (Rev 7):** `idf.py -DBOARD=rev7 build`
- **Flash + monitor:** `idf.py -p /dev/ttyACM0 flash monitor` (USB native to S3)
- **Clean:** `idf.py fullclean` between rev switches (sdkconfig overlay differs)
- **Targets:** Rev 6 (shipping, LSM6DSO32X) and Rev 7 (in design,
  LSM6DSV320X + I2S MEMS mic). Both ESP32-S3-WROOM-1-N16R8.
- **ESP-IDF version:** v6.0+ (upgrade confirmed). v5.5 builds may still work but
  v6.0 is the supported baseline.

### Operational gotchas
- **`eim` is not on PATH.** Source the env directly before any `idf.py`:
  `. $IDF_PATH/export.sh`. `idf.py mcp-server` needs this too.
- **BQ25185 needs a battery present for SYS voltage.** USB-only operation does
  not power the +3V3 rail; a deeply discharged cell forces trickle-charge mode.
  Sequence bring-up with a charged cell attached — complements the charger STAT
  check in the Rev 6 bring-up below.

## When uncertain about ESP-IDF APIs

Use the Espressif Documentation MCP server before answering. If you find
yourself recalling API signatures from training data, stop and query the
MCP instead.

## When uncertain about the LSM6DSV320X

ST's LSM6DSV family is well-documented and the platform driver is
public/permissive. Source of truth, in order:
1. **`boards/rev7/board.h`** — pin map, I2C address, capability flags.
2. **ST datasheet** — `LSM6DSV320X` on st.com. WHO_AM_I, register map,
   ODR options, MLC/FSM/SFLP register layouts, dual-accel routing.
3. **`STMicroelectronics/lsm6dsv320x-pid`** on GitHub — BSD-licensed
   platform-independent driver. Vendor as an ESP-IDF component; do not
   reimplement what's already there.
4. **ST MEMS Studio** — desktop tool for authoring MLC trees, FSM
   programs, and SFLP configuration. Outputs UCF files that the firmware
   loads at boot.

If a specific register address, value, or timing isn't grounded in one of
the above, mark it `// UNVERIFIED` in code rather than guessing.

## Bring-up before features

Rev 6 boards have arrived. The bring-up sketch lives at `main/bringup_rev6/`.
It walks every subsystem and prints PASS/FAIL/SKIP to USB serial:
- I2C scan (expect 0x6A IMU, 0x36 fuel gauge; 0x77 baro only if hand-soldered)
- IMU WHO_AM_I (LSM6DSO32X = 0x6C)
- MAX17048 voltage/SOC sanity check
- Charger STAT pin read
- RFM95W SPI silicon-rev read (if populated)
- GPS UART NMEA presence (if populated)
- BLE advertise + accept connection

Production driver bodies under `main/drivers/` graduate from the bring-up
sketch's verified register sequences, not from speculative recall.

Per-board session findings — what's been observed on real Rev 6 silicon,
what's still open, what's been resolved — live in
`docs/10_rev6_bringup_log.md`. Read it before proposing changes to
`main/bringup_rev6.c` or starting a `main/drivers/` body. Not auto-loaded:
it changes per session and would bloat context if it were. (Firmware-local —
stays in this repo; it is not cross-cutting.)

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
- Don't reimplement what ST's `lsm6dsv320x-pid` / `lsm6dso32x-pid` drivers
  already provide. Vendor them as components, write the platform glue,
  layer our HAL on top.
- Don't add `BOARD_HAS_*` capability flags in CMake. They live in
  `board.h` only; the build system parses them out.

## Sibling repos in this product family

- `onecollar-platform/` — the coordination / brain repo: docs, contracts,
  decision log, `.claude/` tooling. Read its `CLAUDE.md` first for anything
  cross-cutting.
- `OneCollarFirmware/` — V1, legacy, snapshot in the platform repo's `docs/snapshots/`
- `OneCollarHardware/` — KiCad PCB designs (rev 6 shipping, rev 7 in design)
- `WearDoggoTrainer/` — MAUI mobile app, being retired in favor of Flutter
- `WearDoggoTrainerAPI/` — Azure SAS broker (has a known auth gap, see
  kickoff brief, address before broader beta)
- `LastCollarTestBed/` — historical ML/firmware sandbox, snapshot only

## Collaborators

- **Adam Wengert** (you're talking to me) — product, architecture, firmware,
  ML direction
- **Patrick Carberry** — hardware/PCB, engaged via Upwork. Owns final
  Rev 7 PCB pin selection (PATRICK_TODO markers in `boards/rev7/board.h`).

## Security rails (always)

- Never commit secrets. Azure connection strings, API keys, vault names
  go in environment variables or Key Vault references, never source.
- Never hardcode test BLE bond keys or production cert material.
- Never paste live Azure storage keys into code or comments. (V1 has
  done this in `WearDoggoConsole/Program.cs` — do not propagate.)

## When asking me clarifying questions

- Ask at most 3 at a time, prioritized.
- Don't ask about decisions already settled in the kickoff brief
  (`../onecollar-platform/CLAUDE.md` + `decisions/decision-log.md`) — read
  those first.
- Hardware specifics live in `.claude/hardware.md` and `boards/rev*/board.h`,
  not in this file.