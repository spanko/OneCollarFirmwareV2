# Behavior Literature Synthesis

**Date:** 19 April 2026
**Status:** Living reference. Updated as new literature surfaces or architectural decisions shift.
**Supersedes:** ad-hoc research done in conversation on 19 April 2026.

---

## 1. Purpose and scope

This document synthesizes the published literature on IMU-based canine activity recognition, collar-mounted-sensor limitations, canine gait biomechanics, and pose-estimation tooling — as it applies to OneCollar's motion-state-plus-library architecture.

**In scope:**
- Findings that constrain or enable design decisions
- Per-behavior signatures with citations
- Publicly available datasets and pretrained models
- Design implications specific to OneCollar

**Out of scope:**
- Comprehensive literature review (cited works are illustrative, not exhaustive)
- Behaviors outside the initial launch taxonomy
- Patent analysis (handled separately with counsel)

---

## 2. Headline findings

### Static postures are hard from a neck collar
The dominant finding across the literature, consistently reproduced across multiple studies: sit / stand / lie-on-chest are the hardest behaviors to discriminate from a neck-mounted IMU. Kumpulainen et al. measured this directly on 45 medium-to-large dogs wearing sensors simultaneously on a harness (back) and on a neck collar. The harness sensor reached ~91% accuracy; the collar sensor peaked at 75%. The failure mode was specifically static-posture confusion.

Hypothesized causes from the review literature:
- Neck orientation changes minimally between these postures, too subtle for reliable discrimination
- Collar rotation around the neck axis adds noise
- Stanford/Mars-scale data volumes partially mitigate via learned representations

**Implication for OneCollar:** our motion-state-plus-library architecture sidesteps the instantaneous-classification framing entirely. Transition signatures are more discriminative than static endpoints, and the library-matching layer can condition on prior state. This is a structural advantage over classifier-based approaches on this specific problem.

### Rhythmic self-directed behaviors are easy
Vetrax (AGL Technology, commercial) reports 99.24% scratching accuracy and 99.56% head-shake accuracy. Mars/Whistle reports 87% sensitivity and 99.7% specificity for scratching. These behaviors are rhythmic, high-amplitude, and the IMU is physically on the thing doing the shaking. Described in the literature as high-energy repetitive actions that are obvious to both observers and sensors.

### Stride frequency scales with body mass (Heglund 1974)
Gait transition frequencies follow a log-linear relationship with body mass across species from mice to horses. Smaller animals transition at higher stride frequencies. This gives us a principled foundation for per-dog calibration instead of fixed frequency thresholds.

For medium-large launch scope (20-30+ pounds):
- Walking stride frequency: roughly 1.5-2.5 Hz
- Trotting: roughly 2.5-4 Hz
- Galloping: roughly 3-5+ Hz

Exact thresholds need per-dog calibration at setup.

### Collar rotation is partially mitigatable with enough data
Mars/Whistle ran four activity monitors simultaneously on a single collar at different rotational positions and found position did not substantially impact their deep-learning classifier's accuracy. The takeaway: with sufficient labeled data, learned representations absorb rotation invariance. Hand-engineered features should still explicitly address rotation (gravity-aligned frame or rotation-invariant magnitudes) — belt-and-suspenders against the rotation problem.

### Trainable behavior libraries exist as prior art
A 2025 proof-of-concept collar detected a specific trained signaling behavior (two rapid clockwise spins, used by trained assistance dogs to alert seizures) using an IMU plus ML pipeline with 135 labeled alerts from 6 dogs. This is the trainable-library pattern in the wild. Relevant both as validation of the approach and as prior-art awareness for patent counsel.

---

## 3. Per-behavior literature signatures

Covers the launch-scope behaviors and adjacent Tier 2.

### Gait family

Canine locomotion includes six distinct gaits, documented consistently across veterinary references:

- **Walk** — 4-beat asymmetric; slowest; moments of 3 feet on ground
- **Amble** — fast walk; ipsilateral pair advances nearly together but not simultaneously
- **Trot** — 2-beat diagonal with brief suspension; most stable gait; preferred for lameness detection because contralateral limbs never share weight-bearing
- **Pace** — 2-beat lateral (ipsilateral legs together); abnormal in most breeds, normal in a few
- **Canter** — 3-beat asymmetric; energy-conserving; dogs use either rotary or transverse variants
- **Gallop** — 4-beat asymmetric with one or two suspension phases; involves spinal flexion/extension

Gait family is well-benchmarked in the literature and represents the highest-confidence launch behaviors after the rhythmic self-directed family.

### Postural family (the hard problem)

Static postures confusable at the collar per Kumpulainen et al. Distinguishing signal lives in:

- **Lying-on-side** — distinctive roll-axis signature; more separable than sphinx-lie
- **Sphinx-lie, sitting, standing** — gravity-vector direction differs only slightly between these, within rotation-noise margins
- **Transitions between these states** — far more discriminative than endpoints

Costilla et al. working dogs study used 3 IMUs simultaneously (chest, back, neck) across 5 behaviors. The back sensor was consistently the most informative position; accelerometer features dominated gyroscope features in importance ranking.

### Transition family (the library's sweet spot)

Biomechanical patterns from gait/rehabilitation literature and animal biomechanics references:

- **Sit-down (standing → sitting):** rear end lowers first, forequarters stay elevated, pelvis tucks under; roughly 0.5-1.5s duration; neck pitch rises slightly or stays level
- **Lie-down sphinx (standing → sphinx-lie):** forelegs extend or fold, neck drops, body length along ground increases
- **Lie-down side (standing → side-lie):** begins as sphinx, adds roll-axis rotation in second phase
- **Get-up from sit (sitting → standing):** rear rises first, forelegs straighten, brief forward weight shift through collar
- **Get-up from lie (lying → standing):** forelegs typically extend first, then rear pushes up; often involves a spinal arch transient

No published collar-only benchmark for these transitions specifically — this is a literature gap and an opportunity. Our approach to transitions-as-library-entries is novel.

### Rhythmic self-directed family

- **Scratching** — rear leg against head produces strong rotational signal at the neck collar. Vetrax 99.24%, Mars/Whistle 87%/99.7%
- **Head-shake** — rhythmic high-amplitude rotational signal. Vetrax 99.56%
- **Body-shake (shake-off)** — standard category in multiple studies; typically well-classified
- **Digging** — less well-studied than the above; no published accuracy benchmark found in this pass. Distinctive rhythm plus head-down posture; expected to classify well but needs our own measurement
- **Jumping-up / standing-on-hind-legs** — vertical impulse plus large postural excursion; benchmarks unclear but signal components are strong

### Consumption / investigation behaviors

- **Eating** — 98.8% sensitivity, 98.3% specificity (Mars/Whistle); head-lower-and-rhythmic signature
- **Drinking** — 94.9%/99.9% (Mars/Whistle); distinct rhythmic tongue/jaw pattern, slower than eating
- **Licking** — 77.2%/99.0% (Mars/Whistle); noticeably harder, lower amplitude
- **Sniffing** — most distinct behavior in the Helsinki study (Kumpulainen et al.); head-down plus characteristic head-micromovements

### Bathroom (Tier 2)

Not well-studied in the literature reviewed. The vision-doc rationale stands: low-amplitude IMU signature, brief duration, highly posture-variable, strongly per-dog-idiosyncratic. Likely requires targeted data collection and per-dog adaptation heads. Deferred from launch explicitly.

---

## 4. Design implications for OneCollar architecture

### 4.1 The feature set should be biased toward transitions over endpoints
Static-posture classification is a dead end for collar-mounted-only systems — multiple studies confirm this. Our architecture's library-matching layer naturally encodes transitions as prototype traces through feature space. The v0.1 candidate features (neck pitch, neck roll, orientation stability) remain useful as inputs, but temporal-pattern features over those inputs graduate to first-class status.

### 4.2 Rotation invariance is a first-class design constraint
Three-layer treatment:
1. Compute features in gravity-aligned frame where possible (gravity direction is observable from the accelerometer during low-acceleration periods; residual ambiguity is only rotation around the vertical axis)
2. Use rotation-invariant features (magnitudes, ratios, axis-agnostic statistics) where gravity-alignment is uncertain (high-acceleration motion)
3. Allow the learned residual embedding to absorb remaining rotation effects

### 4.3 Per-dog gait calibration is non-optional
Heglund's scaling means a 10 kg dog's walk frequency overlaps a 40 kg dog's trot frequency. Even within the medium-large launch scope this matters. A setup calibration procedure (60-second leash walk captured via phone video) establishes baseline stride frequency per dog. Calibration outputs become library-matching conditioning inputs.

### 4.4 Library entries are traces, not points
Library entry schema should encode:
- **Starting motion state** — as a region in feature space, not a point
- **Transition trace** — sequence of feature vectors over a windowed duration
- **Ending motion state** — region in feature space
- **Required pose keypoints** — for Option B supervision; optional at runtime, required for training
- **Context predicates** — location, time-of-day, recent-history for Blocation fusion

Runtime matching probably uses dynamic time warping or similar sequence-alignment rather than Euclidean distance. TFLite Micro-compatible implementations exist and fit the compute budget.

### 4.5 Option B supervision is directly motivated by the transition problem
Pose keypoints give the ground-truth body geometry that the IMU can only indirectly infer. Multi-task training against pose-derived supervision is the natural fix for the transition-confusion problem. Data collection should capture pose-compatible video *by default*, not as an opt-in — this keeps Option B optionality open across the data we collect from Rev 6 onward.

---

## 5. Public resources we can use immediately

### Datasets
- **Helsinki 6-DOF dog behavior dataset** (Kumpulainen et al.) — 45 medium-to-large dogs, 7 behaviors (sitting, standing, lying, trotting, walking, playing, sniffing), collar + harness sensors, video-validated at 1-second resolution, publicly released at Mendeley Data `10.17632/vxhx934tbn.1`. **Near-perfect match for our Rev 6 sensor configuration.** First-priority validation target.
- **AP-10K** — 10,000+ keypoint-annotated images across 54 mammal species including domestic dogs
- **Stanford Dogs** — classification dataset with breed labels
- **AnimalPose** — keypoint-annotated animal pose dataset

### Pretrained models
- **DeepLabCut SuperAnimal-Quadruped** — 39 keypoints, zero-shot usable on dogs, trained on 40,000+ images across quadruped species. Obsoletes the custom-fine-tune workflow in LastCollarTestBed's `waylon-2025-01-14` DLC project for launch-scope needs.
- **SuperAnimal framework** generally — usable across 45+ species, enabling the multi-species roadmap (cats, horses, cattle) with minimal additional data collection.

### Commercial products as accuracy benchmarks
- **Vetrax / AGL Technology** — scratching and head-shaking accuracy targets (99%+)
- **Whistle FIT / Mars Petcare** — eating, drinking, licking, rubbing, sniffing, scratching accuracy targets
- **ANIMO 3-D** — grooming/scratching/shaking minute-level tracking

We should meet or exceed these baselines for overlapping behaviors and beat them on novel behaviors and library extensibility.

---

## 6. References

### Collar-mounted IMU activity recognition
- Kumpulainen et al. *Dog behaviour classification with movement sensors placed on the harness and the collar* — https://www.sciencedirect.com/science/article/pii/S0168159121001805
- Kumpulainen et al. dataset description — https://www.ncbi.nlm.nih.gov/pmc/articles/PMC8777071/
- Kumpulainen et al. earlier collar-only paper (ACI 2018) — https://dl.acm.org/doi/10.1145/3295598.3295602
- Chambers et al. (Mars/Whistle) *Deep Learning Classification of Canine Behavior Using a Single Collar-Mounted Accelerometer: Real-World Validation* — https://pmc.ncbi.nlm.nih.gov/articles/PMC8228965/
- Costilla et al. *Machine learning based canine posture estimation using inertial data* — https://pmc.ncbi.nlm.nih.gov/articles/PMC10284380/
- Aich et al. *Design of an Automated System for the Analysis of the Activity and Emotional Patterns of Dogs with Wearable Sensors* — https://www.mdpi.com/2076-3417/9/22/4938

### Pruritic behavior detection (scratching / head-shake commercial benchmarks)
- Griffies et al. *Wearable sensor shown to specifically quantify pruritic behaviors in dogs* — https://pmc.ncbi.nlm.nih.gov/articles/PMC5883579/
- Wernimont et al. *Use of Accelerometer Activity Monitors to Detect Changes in Pruritic Behaviors* — https://pmc.ncbi.nlm.nih.gov/articles/PMC5795410/

### Canine gait biomechanics
- Heglund et al. *Scaling stride frequency and gait to animal size: mice to horses* (Science 1974) — https://www.science.org/doi/10.1126/science.186.4169.1112
- Carr & Dycus *Canine Gait Analysis* (Today's Veterinary Practice) — https://todaysveterinarypractice.com/orthopedics/recovery-rehab-canine-gait-analysis/
- *Key Components of Canine Gait Analysis in the Rehabilitation Exam* — https://todaysveterinarynurse.com/rehabilitation/key-components-of-canine-gait-analysis-in-the-rehabilitation-exam/
- *Analyzing the Canine Gait* (TheK9PT) — https://www.thek9pt.com/2016/05/31/analyzing-the-canine-gait/

### Trainable signal detection (prior art relevant to patent positioning)
- *Development and Validation of an IMU Sensor-Based Behaviour-Alert Detection Collar for Assistance Dogs* (MDPI Animals 2025) — https://www.mdpi.com/2076-2615/15/21/3081

### Pose estimation tooling
- SuperAnimal paper — https://www.nature.com/articles/s41467-024-48792-2
- DeepLabCut multi-animal — https://www.nature.com/articles/s41592-022-01443-0
- DeepLabCut lab page — http://www.mackenziemathislab.org/deeplabcut

### LSTM / deep learning on dog activity
- *Long Short-Term Memory (LSTM)-Based Dog Activity Detection Using Accelerometer and Gyroscope* — https://www.mdpi.com/2076-3417/12/19/9427

---

*Drafted 19 April 2026 from a literature search during the first OneCollar behavior-taxonomy working session. Intended to live in the project root alongside `08_kickoff_brief.md` as durable reference material for feature-set design.*
