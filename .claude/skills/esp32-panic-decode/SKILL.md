---
name: esp32-panic-decode
description: Parse and triage ESP32 / ESP32-S3 crash output — Guru Meditation panics, abort()/assert backtraces, stack canary failures, cache-disabled crashes, watchdog (TWDT/IWDT) timeouts, brownout resets, and core-dump output. Trigger when the user pastes panic text containing "Guru Meditation", "Core 0/1 panic'ed", "abort() was called", "assert failed", "Backtrace:", "Stack smashing", "Cache disabled but cached memory region accessed", "Task watchdog got triggered", "Brownout detector was triggered", or raw `0x4...` PC/LR addresses from an ESP32-S3 trace. Also trigger for `.elf` + backtrace decoding requests.
---

# ESP32 / ESP32-S3 panic & backtrace triage

When the user shows panic output, do not skim it. Walk this list. The goal
is: classify the cause → resolve symbols → name the most likely culprit →
propose the next diagnostic step. Don't jump to a fix until the cause class
is named.

## 1. Capture what you have

Ask for / look for, in order:

1. The **full panic block** from the first `Guru Meditation` (or `abort()` /
   `assert failed`) line through the `Backtrace:` line and the reset
   reason. Truncated traces hide the real frame.
2. The **firmware build** that produced it — branch + commit, and the
   matching `.elf` (typically `build/<project>.elf`). Without the same
   ELF, address resolution is guesswork.
3. The **board rev** (rev6 vs rev7 here) — different peripherals fault
   differently.
4. **Reproduction** — every boot? After N hours? Only on Wi-Fi connect?
   Cold vs warm reset? This shapes the hypothesis space more than the
   trace text does.

If any of these are missing, ask before guessing.

## 2. Classify the cause

The first line of a Guru Meditation tells you almost everything. Map the
exception name to a hypothesis class:

| Exception text | Class | Most common cause |
|---|---|---|
| `LoadProhibited` / `StoreProhibited` | Bad pointer deref | Null, freed, or uninitialized pointer; struct accessed after `free()`; PSRAM access while cache is disabled |
| `LoadStoreAlignment` | Misaligned access | `uint32_t*` cast onto an odd address; packed struct field accessed as wider type |
| `IllegalInstruction` | Jumped into garbage | Corrupted function pointer, stack overflow into return address, calling a freed callback, calling code in PSRAM with cache off |
| `InstrFetchProhibited` | PC into unmapped region | Same family as `IllegalInstruction`; often stack smash overwriting LR/PC |
| `Cache disabled but cached memory region accessed` | Flash/PSRAM access during disabled-cache window | Code or data in flash/PSRAM touched from an ISR, from inside `spi_flash_*` ops, or during OTA. Anything called from such a context must be `IRAM_ATTR` and reference only DRAM. |
| `Stack canary watchpoint triggered (taskname)` | Stack overflow | Task stack too small, deep recursion, large on-stack arrays (audio/IMU buffers belong on the heap or in static DRAM). Increase `stack_size` only after measuring with `uxTaskGetStackHighWaterMark`. |
| `Unhandled debug exception` | Hardware breakpoint / watchpoint | Usually a leftover debug watch or a real memory clobber that tripped a guard. |
| `Interrupt wdt timeout on CPU0/1` | ISR or critical section ran too long | Long work in an ISR; `portENTER_CRITICAL` held too long; `vTaskSuspendAll` around blocking work. |
| `Task watchdog got triggered` (TWDT) | Task starved IDLE | A high-priority task spinning without yielding; missing `vTaskDelay`/event wait; tight polling loop. |
| `abort() was called at PC …` | Explicit abort | Failed `assert`, `ESP_ERROR_CHECK` on non-OK return, `configASSERT` in FreeRTOS, C++ uncaught exception. The line above usually names the file. |
| `Brownout detector was triggered` | Power rail dipped | Battery sag under TX load, bad decoupling, undersized regulator. Not a software bug per se — but Wi-Fi/BLE TX bursts are the usual trigger. |
| `Rebooting...` after `Stack smashing protect failure` | GCC `-fstack-protector` tripped | Buffer overflow on stack, almost always `strcpy` / `memcpy` / `sprintf` past a local array. |

Also note **which core** panicked (`Core 0` = PRO_CPU, runs Wi-Fi/BLE by
default; `Core 1` = APP_CPU, app tasks by default in IDF) and **which
task** (`Current task: …`). That alone often points at the subsystem.

## 3. Resolve the addresses

Don't read raw `0x40…` addresses to the user. Resolve them.

The ESP-IDF tool of choice is **`idf.py monitor`**, which already runs
`addr2line` against the current ELF. If the user pasted a trace from
elsewhere:

```bash
# ESP32-S3 (Xtensa). For original ESP32 also xtensa; for C-series use riscv32-esp-elf.
xtensa-esp32s3-elf-addr2line -pfiaC -e build/<project>.elf \
  0x42012abc 0x42013def 0x4200ffff
```

For a richer view (coredump pulled from flash partition or UART):

```bash
idf.py coredump-info     # summary
idf.py coredump-debug    # opens GDB on the dump
```

Notes:
- **The ELF must match the panicking build** down to the commit. A diff of
  even a few lines moves addresses. If the user can't produce the exact
  ELF, say so and stop guessing line numbers.
- For our repo, the ELF is per-board: `build/rev6/...` or `build/rev7/...`
  depending on `BOARD=`. Use the right one.
- `0x4008xxxx` / `0x40378xxx` ranges are typically IRAM/ROM; `0x4200xxxx`
  is flash-mapped instruction; `0x3FCxxxxx` / `0x3FFxxxxx` is DRAM/PSRAM.
  Knowing the region tells you whether a frame is plausibly in user code.

## 4. Read the FreeRTOS context

Inside the panic block, ESP-IDF prints the register dump and often the
**task list** with stack high-water marks. Scan for:

- The **panicking task's name** — does it match the subsystem implicated
  by the backtrace?
- Any task with **`Stack:` near 0** — that's the proximate culprit even if
  the trap fired elsewhere.
- The **`PC` and `EXCVADDR`** registers. `EXCVADDR` is the faulting
  address on Load/StoreProhibited — if it's `0x00000000`, you have a null
  deref; small values (`< 0x100`) usually mean a struct deref via a null
  pointer at a field offset; values in `0x3FC...` that look like a freed
  block address suggest use-after-free.
- `A0` is the saved return address (LR). If the backtrace looks broken,
  resolve `A0` by hand.

## 5. Generate hypotheses, not fixes

After classification + symbol resolution, write a short triage note:

> **Cause class:** StoreProhibited, EXCVADDR=0x00000018
> **Where:** `imu_lsm6dsv320x_read_fifo` at `imu_lsm6dsv320x.c:142`, called
> from task `imu_task` (Core 1, prio 5)
> **Hypothesis (ranked):**
>   1. `s_imu_handle` is NULL — init failed silently and the task wasn't
>      gated on it. Check `imu_lsm6dsv320x_init` return path.
>   2. Handle freed by a re-init on rev-detect change. Race between
>      `bsp_init` and `imu_task` startup.
>   3. Struct layout mismatch between rev6/rev7 build (less likely —
>      would fail earlier).
> **Next step:** add `ESP_ERROR_CHECK` on init, plus a `NULL` guard at
> the top of `read_fifo`, and re-run with `CONFIG_HEAP_POISONING_LIGHT=y`
> to catch use-after-free.

Only write code after the user agrees on the cause class. Panics are
where confident wrong fixes do real damage — they hide the bug for
another month.

## 6. Sanity checks worth running once

When the cause is unclear, suggest enabling (in this order, cheapest first):

1. `CONFIG_FREERTOS_CHECK_STACKOVERFLOW = "Check using canary bytes"` —
   already on by default in IDF; verify.
2. `CONFIG_HEAP_POISONING_LIGHT=y` (or `_COMPREHENSIVE` for nastier
   heap bugs). Catches use-after-free and double-free at the cost of a
   few % CPU.
3. `CONFIG_COMPILER_STACK_CHECK_MODE_NORM` (GCC stack protector).
4. Enable **core dumps to flash** (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`)
   if not already — hugely better than UART traces for intermittent crashes.
5. Add `assert(handle)` at API boundaries of new components — fail loud
   and early at init, never silently with a NULL.

## What NOT to do

- Don't suggest "increase stack size" as a first move on a stack canary
  hit. Measure the high-water mark first; the real bug is often a large
  on-stack buffer that should have been heap or static DRAM.
- Don't recommend disabling the watchdog to "fix" a TWDT timeout. Find
  the task that isn't yielding.
- Don't propose retry loops around `ESP_ERROR_CHECK` failures without
  reading what the failing call returned. `ESP_ERR_NO_MEM` and
  `ESP_ERR_INVALID_STATE` mean very different things.
- Don't trust a backtrace decoded against a different commit. Re-build
  or get the matching ELF.
