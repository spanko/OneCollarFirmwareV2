# Rev 6 model bundles

Tier 0 and Tier 1 model artifacts shipped with Rev 6 firmware.

## Files (none yet)

- `tier0_mlc.ucf` — MLC decision-tree program for the LSM6DSO32X. 8 trees
  max, 256 nodes total. Authored in **ST Unico-GUI**.
- `tier1_cnn.tflite` — Tier 1 IMU CNN, int8 quantized. Trained against Rev 6
  IMU data only (LSM6DSO32X noise/bias characteristics).

## Toolchain notes

Unico-GUI (ST's 2019-era tool) is the supported authoring environment for
LSM6DSO32X UCF files. It has no migration path to LSM6DSV320X — Rev 7 uses
MEMS Studio. Both produce the same on-the-wire UCF format, but the GUIs
target different parts and are not interchangeable.

If we accumulate enough labeled data to retrain Rev 6 Tier 0 trees, this is
where new `.ucf` artifacts land. The driver loads them via
`imu_mlc_load_program()` from a model-partition lookup at boot.
