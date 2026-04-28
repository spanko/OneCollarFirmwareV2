# OneCollar Technical Architecture

**Version:** 0.1 (draft for review)
**Date:** 19 April 2026
**Status:** Living document. Supersedes ad-hoc architecture notes.

---

## 1. Purpose and scope

This document captures the **target technical architecture** for OneCollar Rev 7 and the platform it supports. It is forward-looking — it describes what we are building toward, not a snapshot of what currently exists.

It is a companion to:
- **`01_product_vision.docx`** (formerly "The Last Collar Design Context") — answers *why* and *what*.
- **`03_repo_snapshot.md`** (TBD) — describes what currently exists in the build.

This document answers *how*.

**In scope:**
- Silicon, sensor, and radio selections for the Rev 7 collar
- The four-tier hierarchical inference architecture
- How the "Blocation" concept flows through the ML pipeline
- The training data pipeline and model deployment model
- Framing for the Hub, Phone App, and Cloud tiers

**Out of scope:**
- Detailed schematics, PCB layout, or SkiDL code (lives in the hardware repo)
- API contracts (separate interface specs as they stabilize)
- Operational concerns — CI/CD, observability, cost
- Business and GTM considerations

Anything marked `TBD` is an open decision requiring follow-up.

---

## 2. System overview

OneCollar is a four-domain system:

| Domain | Role | Platform |
|---|---|---|
| **Collar** | Continuous sensing, on-device inference, local decisioning | Custom ESP32-S3 PCB (Rev 7) |
| **Hub** (optional) | Indoor localization, long-range bridge, heavier compute | TBD — §9 |
| **Phone app** | User interface, pairing, mapping, labeling, alerts | Flutter (iOS + Android) |
| **Cloud** | Model registry, training data, fleet management, durable storage | Azure |

The collar is the point of greatest technical difficulty and the patent-defining element. Other tiers are important but far less constrained, and can evolve independently of hardware revisions.

---

## 3. Collar hardware (Rev 7)

### 3.1 Silicon stack

| Function | Part | Status | Notes |
|---|---|---|---|
| MCU + Wi-Fi/BLE | ESP32-S3-WROOM-1-N16R8 | Carry-over | 16 MB flash, 8 MB PSRAM. Enough compute and memory headroom for Tier 1 + Tier 2. |
| IMU | STMicro LSM6DSV320X | **New in Rev 7** | Replaces Rev 6's LSM6DSO32X. Adds dedicated ±320 g high-g channel and chip-native SFLP (gravity vector + game-rotation quaternion) on top of the same 8-tree / 256-node MLC, and exposes 8 programmable Finite State Machines + ASC (Adaptive Self-Configuration). Same I2C address (0x6A) as Rev 6 IMU. |
| Microphone | I2S MEMS (SPH0645LM4H or ICS-43434) | **New in Rev 7** | Enables audio-based behaviors (bark, whine, pant). Acoustic venting required in enclosure. |
| Voice activity detect | Infineon IM73A135 PDM + on-chip VAD | **Evaluating** | Always-on audio wake without MCU duty. Decision pending bench test. |
| LoRa | RFM95W | Carry-over | Long-range property coverage and hub bridge. |
| GPS | NEO-M8Q (external breakout via 6-pin JST-SH) | Carry-over | External board preserves antenna placement flexibility. |
| Battery charger | BQ25185 | Carry-over | Rev 6 JLCPCB DFM issue resolved (EasyEDA footprint). |
| LDO | AP2112K-3.3 | Carry-over | |
| Fuel gauge | MAX17048 | Carry-over | |
| Sub-GHz radio | CC1101 | **Proposed for removal** | Not used by current firmware. Revisit if 802.15.4/Thread becomes required. |
| Barometric | BMP390 | DNP (as Rev 6) | Re-evaluate if altitude-based features earn their power cost. |

### 3.2 Physical constraints
- **Board:** ~80×30 mm, 4-layer stackup (Rev 6 baseline — review for Rev 7)
- **Battery:** LiPo, target 500 mAh — `TBD` pending enclosure decisions
- **Enclosure:** acoustic vent (Gore or equivalent) behind mic; GPS-permissive top surface; IP67 target
- **Attachment:** standard D-ring collar mount

### 3.3 Power targets
- **24-hour average current:** < 5 mA (stretch goal: < 2 mA in home-dominant use)
- **Battery life:** 5–7 days typical; 30+ days geofence-only mode
- Full accounting in §11

---

## 4. Collar firmware

**Framework:** ESP-IDF (carried from Rev 6).

**ML runtime:** TensorFlow Lite for Microcontrollers (TFLite Micro) with ESP-NN / PIE vector acceleration.

**Partition scheme:**
- OTA A/B slots for firmware
- **Dedicated model partition** — model bundles can be updated independently of firmware revs
- NVS for configuration, geofences, behavior metadata, per-dog adaptation weights

**Model bundle format:** TFLite flatbuffer + small JSON manifest (class labels, input shape, tier assignment, version, base-vs-adaptation role). OTA refresh cadence: monthly base models, weekly per-dog adaptation heads.

**Sensor drivers:**
- **STMicro LSM6DSV320X:** MLC + FSM driver; Tier 0 decision trees and state machines loaded at boot from flash. Low-g (±16 g) and high-g (±320 g) channels exposed as parallel data streams; SFLP gravity / game-rotation quaternion read alongside raw IMU.
- **I2S mic:** ringbuffer pattern (ESP-SR), 16 kHz sample rate, fed to Tier 2 on demand
- **GPS:** aggressively duty-cycled, gated by Tier 0 state + geofence policy (§8)

---

## 5. Hierarchical inference architecture

The core of the platform. Four tiers, each escalating compute only when the tier below detects something worth investigating.

### Tier 0 — always-on IMU classification
- **Runs on:** STMicro LSM6DSV320X (main MCU asleep)
- **Input:** 6-axis accel + gyro, 25–50 Hz
- **Classes:** `rest / walk / trot / run / erratic / impact`
- **Model:** MLC decision trees (8 trees, 256 nodes) + FSM (8 programmable state machines, e.g. for impact / collar-off detection), with ASC adapting parameters across motion regimes. The high-g (±320 g) channel feeds the impact branch directly.
- **Power cost:** ~15 µA average
- **Wake trigger:** class transition or sustained `erratic`
- **Role:** activity minutes log; gate for higher tiers; baseline "is the dog doing something?" signal

### Tier 1 — IMU CNN on MCU wake
- **Runs on:** ESP32-S3, woken by Tier 0 interrupt
- **Input:** 2-second IMU window at 100 Hz (1.2 KB) + Blocation feature vector
- **Classes:** `walking / trotting / running / playing / digging / scratching / shaking-off / stairs-up / stairs-down / lying-down / getting-up / jumping-up` (initial set)
- **Model:** 1D CNN, ~50k parameters, int8 quantized, <70 KB flash, <50 ms inference
- **Power cost:** ~8 mA during inference, ~80 ms per event
- **Role:** fine-grained motion classification; output feeds user-facing behavior stream and Blocation rules

### Tier 2 — audio fusion
- **Runs on:** ESP32-S3
- **Input:** 1-second mel-spectrogram (40 bands × 32 frames) + Tier 1 feature vector
- **Classes:** `bark-alert / bark-demand / bark-play / whine / growl / howl / pant-heavy / silence`
- **Model:** 2D CNN ~150k parameters + fusion head ~20k parameters
- **Power cost:** ~25 mA during inference, ~200 ms per event
- **Role:** separates vocal behaviors; fuses with IMU to distinguish context (alert bark vs. play bark vs. demand bark)

### Tier 3 — raw stream to phone
- **Runs on:** BLE stream from collar → Flutter app
- **Input:** ring-buffered IMU + audio, last 30 seconds
- **Trigger:** low-confidence Tier 1/2 event; explicit user request ("what's he doing?")
- **Role:** training data capture; on-demand investigation; label bootstrapping

### Tier escalation policy
Escalation is gated by Tier 0/1 confidence and by configurable policy. Tier 2 does not run for every Tier 1 event — it runs when Tier 1 confidence is low, when Tier 1 classifies `ambiguous`, or when the Tier 0 pattern suggests vocalization (e.g., stationary + rhythmic chest motion). Tier 3 is the exception path, not a steady-state tier.

---

## 6. Blocation fusion

The product vision identifies location+behavior as the central differentiator. Architecturally this means location is **not** a filter applied to a classified behavior — it is a feature input to the Tier 1 classifier itself.

Each Tier 1 inference window carries a compact location encoding alongside the IMU data:

| Feature | Bits | Source |
|---|---|---|
| `indoor_state` | 2 | Wi-Fi SSID presence + BLE beacon presence |
| `zone_id` | 8 | Active geofence zone (0 = unknown/elsewhere) |
| `proximity_flags` | 4 | In exclusion zone, approaching boundary, at home, lost |
| `time_of_day_bucket` | 3 | 8 buckets (midnight/morning/afternoon/etc.) |

The Tier 1 model sees these as additional input channels. This lets the classifier learn conditional behaviors directly (e.g., "bark while in kitchen" becomes a distinct behavior-context class without requiring per-zone models or rigid rule layers).

Geofence rules over classified outputs are still supported for user-facing automation — "alert me if he's in `exclusion_zone_pond`," for instance — but the classifier itself is context-aware.

Zone and policy definitions live in the NVS partition and are pushed from the app on change.

---

## 7. Behavior training pipeline

The platform's patent-defining capability is **trainable behavior extension** — new behaviors deployable to the fleet without firmware rev, and per-dog personalization over a base model.

### 7.1 Training data sources
1. **Bootstrap corpus:** public datasets (Stanford CRNN dog activity set, UCSD/Cornell IMU-on-dog papers), augmented with rotation, time-warping, sensor-noise injection
2. **In-field labeled events:** Flutter app timeline labels from beta + production users
3. **Low-confidence ring-buffer captures:** Tier 3 events streamed to cloud and queued for labeling
4. **Synthetic augmentation:** simulated IMU data for underrepresented behaviors and collar orientations

### 7.2 Label capture UX
The Flutter app surfaces an "interesting moments" daily timeline. User taps an event, picks a label from a palette, syncs to cloud. Unlabeled events age out after a configurable window.

Active labeling mode: user taps "he's about to [go to the bathroom] — capture the next 30 seconds." This produces high-quality supervised data for hard-to-elicit behaviors.

### 7.3 Training infrastructure
- **Initial pipeline:** Edge Impulse (fast to iterate, ESP32-S3 export supported)
- **Migration path:** custom Azure ML pipeline once model complexity or data volume exceeds Edge Impulse's sweet spot
- **Retraining cadence:** base models monthly; per-dog adaptation heads weekly
- **Validation:** per-dog held-out set + aggregate fleet metrics

### 7.4 Base model + adaptation head pattern
A **base model** (~50k params, ~70 KB) handles the fleet — trained on aggregated labeled data across all dogs.

A **per-dog adaptation head** (~5–10k params, ~20 KB) fine-tunes on an individual dog's labeled data. This is where idiosyncratic patterns get solved — "my dog poops weird," "my Lab does this odd jumping thing" — without retraining the base.

The adaptation head is fine-tuned in the cloud initially. On-device fine-tuning is a future capability (§13).

### 7.5 New-behavior deployment workflow
Adding a behavior class to the fleet is a workflow, not a firmware rev:

1. Curate labeled examples (from fleet captures, active labeling, or synthetic)
2. Retrain base classifier with new class head
3. Publish updated model bundle to cloud registry
4. OTA push to opt-in collars
5. Promote to default once quality threshold met

Extending to other species (cats, horses, cattle — per vision doc) follows the same workflow with a species-specific base model.

---

## 8. Radio and connectivity

| Radio | Primary use | Range | Power profile |
|---|---|---|---|
| BLE | Phone pairing, event streaming, labeling, Tier 3 capture | ~30 m | Low at advertising; moderate when streaming |
| Wi-Fi | Home sync (model OTA, event batch upload), indoor presence | ~50 m | High; duty cycled, only at home |
| LoRa | Out-of-range tracking, hub bridge, long-range property coverage | 1–10+ km | Very low duty cycle |
| GPS | Outdoor localization when policy requires | N/A | **Dominant power sink. Aggressive gating is non-negotiable.** |

**Gating policy:**
- **At home** (Wi-Fi SSID present or BLE hub in range) → GPS off, opportunistic BLE/Wi-Fi sync
- **Outside home** (geofence exit) → GPS on (duty cycled), LoRa as backup
- **Lost** (no known Wi-Fi, no BLE, no GPS fix for N minutes) → LoRa distress beacon

---

## 9. The Hub (deferred)

Placeholder. Open questions:

- Is a hub required for Rev 1 product, or nice-to-have?
- Hardware platform: Linux SBC (RPi CM4, Rockchip) or custom?
- Role in ML: does the hub offload Tier 2 (heavier audio classifiers than fit on collar)?
- Indoor localization approach: BLE RSSI triangulation? UWB? Something simpler?

Not on the Rev 7 silicon critical path. Revisit after Rev 7 bring-up.

---

## 10. Phone app

**Tech stack:** Flutter (iOS + Android from one codebase).

**Key surfaces:**
- Pairing and onboarding
- Map view with geofence editor (home fence, exclusion zones, ad-hoc fences)
- Behavior event timeline (filterable by day, location, class)
- Label capture UX (§7.2)
- Alerts and push notifications
- Multi-dog view (household-level)
- Longitudinal health trends (gait changes, activity trends, sleep patterns — §13)

**Design principle:** offline-first. Local event store with background sync.

---

## 11. Power budget

Target: **< 5 mA 24-hour average.**

| Sink | Average current | Assumptions |
|---|---|---|
| Tier 0 IMU always-on | ~15 µA | LSM6DSV320X with MLC + FSM in low-power classifier mode |
| Tier 1 inference | ~15 µA | 200 events/day × 8 mA × 80 ms |
| Tier 2 inference | ~2 µA | 30 events/day × 25 mA × 200 ms |
| BLE advertising | ~200 µA | 1 Hz adv interval |
| Wi-Fi sync bursts | ~50 µA | 2 min/day at home |
| LoRa beacon | ~30 µA | 1 beacon/hour when lost |
| **GPS (home-gated)** | ~2 mA | ~1 hour/day of active fixing |
| **GPS (away-heavy)** | ~10+ mA | Continuous 1 Hz fixes — opt-in mode |

**Typical home-dominant:** ~2.5 mA avg → 500 mAh → ~8 days
**Active outdoor:** ~8 mA avg → 500 mAh → ~2.5 days

GPS duty cycle is the dominant variable. Every other optimization is a rounding error relative to it.

---

## 12. Cloud

**Platform:** Azure (aligned with existing infrastructure competence).

**Services:**
- **Model registry** — versioned model bundles, metadata, deployment policies, rollback
- **Data lake** — event captures, labels, telemetry (Cosmos DB + Blob Storage)
- **Training pipeline** — Azure ML or container-based; initially Edge Impulse cloud
- **Fleet management** — device registry, OTA orchestration, health telemetry, diagnostics
- **Auth** — Entra External ID for app users

**Privacy principle:** no persistent raw audio stored by default. Only labeled or explicitly opt-in samples are retained. Raw audio from Tier 3 captures is transient — it exists long enough to be labeled or discarded.

---

## 13. Open decisions and TBDs

| # | Decision | Owner | Blocking? |
|---|---|---|---|
| 1 | Bench-test Infineon IM73A135 PDM VAD vs. LSM6DSV320X MLC-driven wake (motion-triggered audio capture) — i.e. is a dedicated always-on audio path worth the BOM cost when the IMU can already gate I2S sampling? | AW | Rev 7 |
| 2 | Confirm CC1101 can be removed (no firmware dependency) | AW | Rev 7 |
| 3 | Battery size and enclosure finalization | AW + mechanical | Rev 7 |
| 4 | Hub scope: required for v1 or v2? | AW | No (post-Rev-7) |
| 5 | Edge Impulse → custom training pipeline cutover threshold | AW | No (infrastructure) |
| 6 | Patent claim chart against Halter (US11944070, US11937578) | Patent counsel | Filing |
| 7 | On-device adaptation-head fine-tuning (future) | Future | No |
| 8 | Tier 1 class list finalization (current draft is a starting point) | AW | Rev 7 firmware |
| 9 | Longitudinal health trend analytics (hip dysplasia detection from getting-up kinematics) — first-class feature or v2? | AW | No |
| 10 | Species extension roadmap (cat, horse, cattle) — fleet separation strategy | AW | No (long-term) |

---

*v0.1 — drafted 19 April 2026 based on vision doc, Rev 6 design context, and hierarchical inference discussion. Requires review before circulation.*
