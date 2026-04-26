# Rev 7 model bundles

Tier 0 and Tier 1 model artifacts shipped with Rev 7 firmware.

## Files (none yet)

- `tier0_mlc.ucf` — MLC decision-tree program for the LSM6DSV320X. 8 trees,
  256 nodes (same capacity as Rev 6 DSO32X). Authored in **ST MEMS Studio**.
- `tier1_cnn.tflite` — Tier 1 IMU CNN with audio fusion, int8 quantized.
  Input vector includes SFLP-derived gravity components and audio features
  not available on Rev 6.
- `tier2_audio.tflite` — Tier 2 audio classifier (Rev 7 only).

## Toolchain notes

MEMS Studio is the active ST tool for LSM6DSV320X. It supersedes Unico-GUI
and adds support for SFLP and FSM editing alongside MLC training. The UCF
output format is the same across both tools; only the GUI and the supported
parts differ.

A Rev 7 model retrained against the larger combined Rev 6 + Rev 7 data lake
gets dropped here. The data lake's session capability flags
(`DATALOG_CAP_IMU_SFLP`, `DATALOG_CAP_AUDIO`, `DATALOG_CAP_IMU_HIGHG`) let
the training pipeline filter sessions appropriately — Rev 7 audio models
only see Rev 7 sessions; gravity-feature models can use both rev sources
with appropriate per-source weighting.
