# OneCollar Behavior Engine — Kickoff Brief

**Date:** 19 April 2026
**Purpose:** Orient any new conversation in this project. Read this first.

---

## What this project is

OneCollar is a smart dog collar platform. The hardware matters, but the platform's defining capability is a **trainable behavior library built on continuous motion-state estimation** — not fixed-class behavior classification.

Users teach the collar new behaviors by capturing synchronized video of their dog doing the thing they care about. The platform learns to recognize that motion pattern, flag it when it occurs, and optionally trigger user-defined responses. Behaviors can be named ("loping"), aliased to other behaviors ("running-slowly"), or left as abstract motion signatures.

Scope: hardware, firmware, on-device ML, training pipeline, mobile app, cloud. Out of scope: SGS, consulting work, fantasy golf, and other unrelated threads.

---

## The reframe that shaped v0.2 architecture

v0.1 treated the on-device model as a behavior classifier (walking, running, barking, etc.). v0.2 is different:

**Motion-state estimation and behavior recognition are decoupled.**

- On-device model produces a **hybrid motion-state representation**: 6–10 interpretable named features (activity intensity, postural angle, vocalization intensity, gait frequency, etc.) plus a learned residual embedding. Heavily weighted toward the interpretable layer early; the residual earns its keep over time.
- **Behavior recognition is a separate library-matching layer** that runs on top of motion-state estimates. Library entries contain prototype motion-state traces and/or rule predicates.
- **Novelty detection** — flagging motion states that don't match any library entry with confidence — is the hardest and most important capability on the platform. Staged rollout with conservative thresholds at launch.
- Classification of fixed behavior classes is available as a **fallback** if motion-state-plus-library doesn't meet quality targets.

This is the patent-defining architecture. Adding a behavior to the fleet is a library update, not a model retrain. Extensibility to other species (cats, horses, cattle) follows the same pattern with a species-specific base.

---

## Decisions already made

| Area | Decision |
|---|---|
| **Mobile framework** | Flutter. MAUI retired as prior art for BLE protocol, data contracts, and UX patterns. |
| **Firmware** | OneCollarFirmwareV2 (clean ESP-IDF rebuild, currently 3 files, no commits) becomes the Rev 7 firmware. Retarget from Rev 5-S3 to Rev 7 hardware. |
| **Firmware repo structure** | TODO: restructure to house multiple hardware revisions in one repo, matching OneCollarHardware's pattern. No more `OneCollarFirmwareV3`. |
| **BLE protocol** | Clean-sheet design on latest SDKs. v2 reliability layer (SACK, flow control, fragmentation) as inspiration — rebuild if good, don't carry code forward. |
| **Rev 7 silicon** | Keep ESP32-S3-WROOM-1-N16R8, swap IMU to TDK ICM-45685, add I2S MEMS mic, evaluate Infineon PDM+VAD, drop CC1101 pending firmware check. |
| **Training pipeline** | **Option C.** Edge Impulse for fast unblock (Rev 7 gets a trained model shipped). Azure-native pipeline as target state built in parallel. Explicit metric-based cutover criteria, not calendar-based. |
| **Data strategy** | Restart data collection with richer schema. Rev 6 becomes the intentional data-gathering platform; Rev 7 retrains against new sensors when it ships. |
| **Training signal** | Option A (supervised classification from labels) as current approach. **Option B (multi-task learning with pose supervision) preserved as explicit evaluation track** — data collection must not close off B. |
| **VideoIMU synchronization** | First-class training infrastructure. Every session captures synchronized IMU + audio (Rev 7 only) + phone video + extracted pose keypoints. |
| **Pose estimation** | Pluggable, cloud-side, versioned. Re-extractable over time as better pose models emerge. Zero commitment to DLC, ViTPose, or any specific framework. |
| **Simulation-based evaluation** | First-class platform component. Compares Option A vs. Option B on accuracy, speed, effort, novelty-detection quality, few-shot behavior addition. |

---

## Open design questions for the first working session

Three grounding decisions that should be settled before v0.2 prose is written:

1. **Behavior taxonomy.** What behaviors does the platform detect at launch? What's Tier 2? What's research-only? Grouped by difficulty, with explicit gaps called out (e.g., "going to the bathroom" is a hard temporal-pattern problem).
2. **The interpretable feature set.** Name the 6–10 features. For each: what sensors produce it, what range it covers, how it's computed, what behaviors it discriminates.
3. **Novelty detection v1 policy.** Conservative threshold definition, temporal smoothing approach, user-facing behavior when novelty is flagged, false-positive budget.

---

## Recommended first project session

**Pressure-test the behavior taxonomy and motion-state feature set.** Walk through each target launch behavior and decompose it into: sensor signatures, required interpretable features, library-matching approach, data-collection needs. Identify behaviors that expose gaps in the feature set, iterate.

This grounds v0.2 in concrete design rather than handwaving. After that session, v0.2 architecture doc writes itself.

---

## Context for a new chat

**Hardware current state:** Rev 6 shipping (ESP32-S3 + LSM6DSO32X + RFM95W LoRa + external NEO-M8Q GPS, 80×30mm 4-layer). Rev 7 in design.

**Firmware current state:** OneCollarFirmwareV1 works but has architectural debt (two BLE service implementations compiled in, three comm_manager variants, renumbered command opcodes, protocol drift between firmware and app). OneCollarFirmwareV2 is a clean-start scaffold — three files, no commits. SmartCollarBase (hub) is a heartbeat-loop scaffold.

**Mobile current state:** WearDoggoTrainer (MAUI) has working BLE, working Azure data layer, broken Microcharts, half-built VideoIMU sync. Will be retired. Flutter rewrite starts fresh, carries forward only the protocol knowledge.

**Cloud current state:** Azure Blob + Tables + Key Vault + WearDoggoTrainerAPI (SAS broker) working. **Security issue flagged:** `GET /api/blob/get-datatable-connection-string` returns the storage connection string unauthenticated with `AllowAnyOrigin` enabled. Plus a possibly-live Azure Storage key embedded in `WearDoggoConsole/Program.cs`. Address before broader beta distribution.

**ML current state:** LastCollarTestBed has three parallel classifier architectures (boosted tree, perceptron, LSTM) and Jupyter notebooks for training. None integrated with the Azure data lake or with any deployed collar. Historical work only — the new training loop starts fresh with the richer session schema.

**Patent context:** Halter (US11944070, US11937578) is prior art to navigate around. The motion-state-estimation-plus-trainable-library architecture is the claim-defining element. Worth an explicit counsel conversation when v0.2 is stable.

**Collaborators:**
- **Patrick Carberry** — hardware collaborator, engaged via Upwork.
- **Adam Wengert** — product, architecture, firmware, ML direction.

---

*Drafted 19 April 2026. Supersedes conversation history for onboarding purposes. Update as decisions change.*
