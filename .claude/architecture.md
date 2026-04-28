# OneCollar Architecture Reference

Loaded automatically via `@./.claude/architecture.md` from CLAUDE.md.
This is the "do not regress" reference. Every session sees it.

## The decoupling principle (hard rule)

The on-device model produces a **motion-state representation**, not behavior
class probabilities. Behavior recognition is a separate library-matching
layer that runs on top.

Motion-state vector per inference window:
- 6-10 **named interpretable features** (activity intensity, postural angle,
  gait frequency, vocalization intensity, etc.) — heavily weighted at launch
- A **learned residual embedding** — earns its keep over time as fleet data grows

Library entries match against this vector. They are NOT class heads on the model.

**Why this matters:** adding a behavior is a library update, not a model
retrain. Per-dog adaptation is a library specialization. Cross-species
extension reuses the architecture with a species-specific base. Novelty
detection is structurally possible.

Architectures that fold motion-state and behavior recognition back together
foreclose all of the above. Flag any such proposal explicitly.

## Hierarchical inference (4 tiers)

| Tier | Runs on | Trigger | Power | Output |
|------|---------|---------|-------|--------|
| 0 | ICM-45685 SmartMotion (Rev 7) / LSM6DSO32X MLC (Rev 6) | Always-on | ~15 µA | Activity class, wake gate |
| 1 | ESP32-S3 woken by Tier 0 | Class transition or sustained activity | ~8 mA × 80 ms/event | Motion-state vector |
| 2 | ESP32-S3 + I2S mic (Rev 7 only) | Tier 1 ambiguity or vocalization signal | ~25 mA × 200 ms/event | Audio-fused motion-state |
| 3 | BLE stream to phone | Low-confidence escalation or user request | High but rare | Raw 30-sec ring buffer for capture/labeling |

Escalation is gated by Tier N confidence. Tier 2 does NOT run for every Tier 1
event. Tier 3 is the exception path, not steady-state.

## Blocation: location as a feature input, not a filter

Location+behavior fusion is implemented by feeding location encoding directly
into the Tier 1 model as additional input channels:

- `indoor_state` (2 bits) — Wi-Fi SSID + BLE beacon presence
- `zone_id` (8 bits) — active geofence zone (0 = unknown)
- `proximity_flags` (4 bits) — exclusion zone, approaching boundary, at home, lost
- `time_of_day_bucket` (3 bits) — 8 daily buckets

The classifier learns context-conditional motion patterns directly. Geofence
rules over classified outputs are still supported for user-facing automation
("alert me if he's in `exclusion_zone_pond`"), but the model itself is
context-aware. Do not propose location as a post-classification filter layer.

## Library entry schema

Library entries encode **traces through feature space**, not single points:

- Starting motion state — region, not point
- Transition trace — sequence of feature vectors over a windowed duration
- Ending motion state — region, not point
- Required pose keypoints — Option B supervision; optional at runtime
- Context predicates — for Blocation conditioning

Runtime matching uses sequence alignment (DTW or similar), not Euclidean
distance to a centroid. TFLite-Micro-compatible implementations exist.

## Option A vs Option B (training signal)

- **Option A (current)**: supervised classification from user labels.
  Sufficient for launch-scope rhythmic behaviors.
- **Option B (aspirational)**: multi-task learning with pose-keypoint
  supervision derived from synchronized phone video. Directly addresses
  static-posture confusion (Kumpulainen et al.: collar-only classifier
  caps at ~75% accuracy on sit/stand/lie).

**Data capture MUST preserve Option B reachability.** Every session captures
synchronized IMU + audio (Rev 7) + phone video at full fidelity. Windowing
happens at training time, not at capture time. Pose extraction is cloud-side
and re-runnable as better pose models emerge.

Flag any proposal that closes off Option B: discarding raw streams after
Tier 2 inference, downsampling at capture, only retaining label-aligned windows.

## Novelty detection (first-class capability, not future feature)

Conservative threshold-based at launch. Library-match confidence below
threshold → "unknown motion observed" event in the timeline, no automation
triggered. False-positive budget defined per-user; novelty events can be
labeled by user to grow their library.

Design choices must preserve the ability to flag unknown motion states for
user review without flooding them with noise. Open-set recognition and
learned-anomaly approaches are tracked in `09_behavior_literature_synthesis.md` §8
for v2 and beyond.

## Patent positioning

Halter (US11944070, US11937578) is prior art to navigate around. The
motion-state-plus-trainable-library architecture is the claim-defining
element. Counsel review when v0.2 architecture is stable. Never write code
or comments that suggest copying from competitor implementations.
