# Rev 6 Bring-up Log

**Status:** Living doc. Updated as boards are exercised and findings are
resolved. Self-contained — issues are tracked here, not in GitHub, so the
repo carries its own state.

This is the canonical home for "what's been observed on real Rev 6
silicon and what's still open." Production driver bodies under
`main/drivers/` should not be written until the relevant finding here
either passes cleanly or is marked WONTFIX with rationale.

---

## How to use this doc

- **Open findings** are actionable. Each has a reproduction, hypothesis,
  and suggested fix. Code TODOs reference the finding's anchor
  (e.g. `// TODO(bringup-log:cc1101-sres)`).
- **Resolved findings** stay in the doc with the resolving commit hash —
  they're the audit trail for why drivers look the way they do.
- **Per-board sessions** are appended chronologically below. Cross-board
  patterns get promoted to the "Open findings" section at the top.

---

## Open findings

### `cc1101-sres` — CC1101 register reads return zeros after `CC1101 RESET`

**Observed**
After `CC1101 ID` the chip reports
`PARTNUM=0x00 VERSION=0x14 MARCSTATE=0x01` (good). After `CC1101 RESET`
and a follow-up `CC1101 ID`, the same reads return
`PARTNUM=0x00 VERSION=0x00 MARCSTATE=0x00`.

**Expected**
PARTNUM=0x00 (unchanged), VERSION=0x14 (silicon ID — unchanged), and
MARCSTATE consistent with the post-SRES IDLE state (0x01).

**Hypothesis**
After an SRES strobe the CC1101 transits through reset and powers on
its crystal oscillator. Per TI CC1101 datasheet §19.4 the host must
pull CSn low and wait for MISO to go low (`CHIP_RDY`) before issuing
any subsequent command. The current strobe path doesn't poll for
CHIP_RDY — it just issues a fixed `vTaskDelay(5ms)` after deasserting
CSn, then returns. The next register read drives CSn low and starts
clocking SCK before the chip is actually ready, returning floating-line
zeros.

**Repro**
1. Flash Rev 6 bring-up: `idf.py -DBOARD=rev6 -DBRINGUP=ON build flash monitor`
2. REPL: `CC1101 ID` → confirm `VERSION=0x14`.
3. REPL: `CC1101 RESET`.
4. REPL: `CC1101 ID` → reads `VERSION=0x00`.

**Suggested fix**
In `cc1101_strobe()` / the read path, after CSn=low, poll MISO until
it's low (or timeout, ~150 µs is the documented max) before clocking
the address byte out. Equivalently: in `cmd_cc1101_reset()`, after the
SRES delay, do a dummy bus transaction that waits for CHIP_RDY before
returning. Reference TI design note DN503 for the canonical sequence.

**Files**
- `main/bringup_rev6.c:663-666` — `cmd_cc1101_reset()`
- `main/bringup_rev6.c:366` — `cc1101_strobe()`
- `main/bringup_rev6.c:346-348` — register cite block

---

### `chgr-en-n-readback` — `STATUS` readback of `CHGR_EN_N` always reads 0 regardless of drive state

**Observed**
After `CHG DIS` (which prints `Charger DISABLED: CHGR-EN-N driven HIGH`),
the subsequent `STATUS` line reads `CHGR_EN_N=0`. After `CHG EN`
(prints `driven LOW`), STATUS also reads `CHGR_EN_N=0`. The readback
never reflects the driven output level.

**Expected**
`CHGR_EN_N=1` after `CHG DIS`, `CHGR_EN_N=0` after `CHG EN`.

**Hypothesis**
`cmd_chg()` configures GPIO21 with
`gpio_set_direction(..., GPIO_MODE_OUTPUT)`. On ESP32-S3,
`GPIO_MODE_OUTPUT` disables the pin's input buffer (IE=0).
`print_status()` then calls `gpio_get_level(BOARD_CHARGER_EN_N_GPIO)`,
which reads the GPIO input register — but with IE=0 that register is
not driven by the pad, so it reads 0 regardless of the output level.

**Repro**
1. Flash Rev 6 bring-up (USB-powered, battery state irrelevant for the
   readback question).
2. REPL: `CHG DIS` → message says "driven HIGH".
3. REPL: `STATUS` → reports `CHGR_EN_N=0`.
4. REPL: `CHG EN` → message says "driven LOW".
5. REPL: `STATUS` → still reports `CHGR_EN_N=0`.

**Suggested fix**
Change the direction in `cmd_chg()` to `GPIO_MODE_INPUT_OUTPUT` so the
input buffer stays enabled and `gpio_get_level()` reflects the actual
pad level. Verify with a scope on GPIO21, or by observing BQ25185 STAT
behavior change across `CHG EN` / `CHG DIS` once a battery is fitted.

**Files**
- `main/bringup_rev6.c:725-732` — `cmd_chg()`
- `main/bringup_rev6.c:762-764` — STATUS readback

**Related**
- `boards/rev6/board.h` — `BOARD_CHARGER_EN_N_GPIO` definition (was
  `BOARD_CHARGER_STAT_GPIO` before `05d88d1`).

---

## Resolved findings

### `lsm6dso32x-fs-xl-scale` — IMU reading half-scale at rest (`|a| ≈ 0.5 g`) — **fixed in `33d4988`**

**Was**
`CTRL1_XL = 0x48` (FS_XL = 0b10) combined with
`LSM6_ACCEL_G_PER_LSB = 0.122 mg/LSB` produced `|a| ≈ 0.504 g` for a
stationary board. Expected `|a| ≈ 1.000 g`.

**Root cause**
The LSM6DSO32X uses a different `FS_XL` encoding from the regular
LSM6DSO. On the 32X, `00=±4g, 01=±32g, 10=±8g, 11=±16g`. Writing
`FS_XL=0b10` selected ±8 g (LSB = 0.244 mg), but the firmware scaled
by the ±4 g constant — halving every reading. Verified against
`STMicroelectronics/lsm6dso32x-pid` `lsm6dso32x_fs_xl_t`.

**Fix**
Changed `CTRL1_XL` write to `0x40` (`FS_XL=0b00 → ±4 g`). Live
verification with the host TUI showed `|a| ≈ 1.005 g` at rest.

---

## Session log

### 2026-06-05 — Board `44:1B:F6:D9:E5:04` (first Rev 6 silicon)

**Setup**
- ESP-IDF v6.0
- Build: `idf.py -DBOARD=rev6 -DBRINGUP=ON build flash`
- USB-powered, no battery connected.
- WSL2 + usbipd-win with `--auto-attach` (see "Workflow gotchas" below).

**Walk result: 10 PASS, 0 FAIL, 0 SKIP**

| Check | Result | Notes |
|---|---|---|
| NVS init | PASS | |
| I2C init | PASS | port=0 sda=2 scl=1 @ 100 kHz |
| I2C scan | PASS | 0x36 (fuel) + 0x6A (IMU); baro correctly DNP |
| LSM6DSO32X WHO_AM_I | PASS | 0x6C |
| MAX17048 | PASS | 4.202 V, SOC 101.93% (uncalibrated, expected) |
| Charger | PASS | GPIO21 driven LOW (CE asserted, charger enabled) |
| RFM95W RegVersion | PASS | 0x12 |
| CC1101 PARTNUM/VERSION | PASS | 0x00 / 0x14 |
| GPS NMEA on UART1 | PASS | TX=IO9 RX=IO10 @ 9600 8N1; no fix indoors |
| BLE advertise | PASS | as "OneCollar-Bringup" |

**REPL exercise — everything tested**
`STATUS`, `PINS`, `I2C` (100k + 400k), `I2C LEVELS`, `I2C FIND` (NORMAL
+ SWAP both confirmed correct), `FUEL`, `IMU INIT`, `IMU READ` ×2,
`GPS READ 3000` ×2, `GPS RESET`, `LORA VER` + `LORA RESET` + re-read,
`CC1101 ID` + `CC1101 RESET` + re-read (this surfaced `cc1101-sres`),
`CHG DIS` + STATUS + `CHG EN` + STATUS (this surfaced
`chgr-en-n-readback` and revealed the IMU `lsm6dso32x-fs-xl-scale`
issue downstream of host-side `IMU STREAM` visualization).

**Host-side tooling shipped this session**
- `tools/imu_viz.py` — terminal TUI of the 6-axis IMU at 20 Hz
  (commit `3be7869`)
- `tools/imu_viz_3d.py` — rerun web viewer with complementary-filter
  orientation + strip charts (commit `3be7869`)

**Workflow gotchas observed**
- **usbipd auto-attach is required.** Default `usbipd attach --wsl`
  is one-shot; every chip reset (boot, `idf.py flash`'s final
  hard-reset, esp_restart) re-enumerates the USB device and WSL drops
  it. `usbipd attach --wsl --busid <id> --auto-attach` survives all of
  the above and is non-negotiable for productive bring-up sessions.
- **`pyserial` default DTR/RTS = True is the right setting** on the
  S3 USB-Serial/JTAG. Forcing them to False on `open()` (in an attempt
  to "play it safe") actually triggers a chip reset. Documented in
  `tools/imu_viz.py` / `tools/imu_viz_3d.py` constructor — keep
  defaults.

**Expected-but-worth-noting non-bugs**
- With no battery connected, `CHG DIS` collapses the BQ25185 BAT-pin
  rail that powers the MAX17048 → fuel gauge stops responding. This
  is expected (and was momentarily misread as a power-tree bug before
  the no-battery state was recalled). `CHG EN` brings the rail back
  and the fuel gauge responds again.
- GPS NMEA shows V status / 0 satellites indoors — also expected.

---

*Drafted 2026-06-05 after the first Rev 6 bring-up session. Append new
sessions chronologically below this line. Promote cross-session patterns
to the "Open findings" section at the top.*
