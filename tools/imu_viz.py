#!/usr/bin/env python3
"""Live IMU visualizer.

Sends `IMU STREAM ON` to the Rev 6 bring-up REPL and renders accel + gyro
axes as horizontal bars in the terminal. Ctrl-C stops streaming cleanly.
"""

import argparse
import math
import re
import signal
import sys
import time

import serial

LINE_RE = re.compile(
    r"A\[g\]\s+x=([-+\d.]+)\s+y=([-+\d.]+)\s+z=([-+\d.]+)\s+\|\s+"
    r"G\[dps\]\s+x=([-+\d.]+)\s+y=([-+\d.]+)\s+z=([-+\d.]+)"
)

ACCEL_RANGE_G = 4.0
GYRO_RANGE_DPS = 500.0
BAR_WIDTH = 40

ANSI_HOME = "\x1b[H"
ANSI_CLR_SCREEN = "\x1b[2J"
ANSI_CLR_LINE = "\x1b[K"
ANSI_HIDE_CURSOR = "\x1b[?25l"
ANSI_SHOW_CURSOR = "\x1b[?25h"


def bar(value: float, vmax: float, width: int = BAR_WIDTH) -> str:
    half = width // 2
    frac = max(-1.0, min(1.0, value / vmax))
    n = int(round(frac * half))
    cells = [" "] * width
    cells[half] = "|"
    if n >= 0:
        for i in range(half + 1, half + 1 + n):
            cells[i] = "█"
    else:
        for i in range(half + n, half):
            cells[i] = "█"
    return "[" + "".join(cells) + "]"


def render(ax, ay, az, gx, gy, gz, hz):
    a_mag = math.sqrt(ax * ax + ay * ay + az * az)
    tilt_deg = math.degrees(math.acos(max(-1.0, min(1.0, az / a_mag)))) if a_mag > 0.05 else float("nan")
    lines = [
        "OneCollar Rev 6 — IMU live (Ctrl-C to stop)",
        f"  rate:   {hz:5.1f} Hz   accel range ±{ACCEL_RANGE_G:.0f} g   gyro range ±{GYRO_RANGE_DPS:.0f} dps",
        "",
        f"  ax {ax:+6.3f} g    {bar(ax, ACCEL_RANGE_G)}",
        f"  ay {ay:+6.3f} g    {bar(ay, ACCEL_RANGE_G)}",
        f"  az {az:+6.3f} g    {bar(az, ACCEL_RANGE_G)}",
        "",
        f"  gx {gx:+7.1f} dps  {bar(gx, GYRO_RANGE_DPS)}",
        f"  gy {gy:+7.1f} dps  {bar(gy, GYRO_RANGE_DPS)}",
        f"  gz {gz:+7.1f} dps  {bar(gz, GYRO_RANGE_DPS)}",
        "",
        f"  |a|     {a_mag:5.3f} g   (1.000 = at rest)",
        f"  tilt z  {tilt_deg:5.1f} deg from vertical",
    ]
    out = [ANSI_HOME]
    for ln in lines:
        out.append(ln + ANSI_CLR_LINE + "\n")
    out.append(ANSI_CLR_LINE)
    sys.stdout.write("".join(out))
    sys.stdout.flush()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    s = serial.Serial()
    s.port = args.port
    s.baudrate = args.baud
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    time.sleep(0.3)
    s.reset_input_buffer()

    s.write(b"IMU INIT\r\n")
    time.sleep(0.3)
    s.reset_input_buffer()
    s.write(b"IMU STREAM ON\r\n")
    time.sleep(0.2)

    stop = {"flag": False}

    def _sigint(signum, frame):
        stop["flag"] = True

    signal.signal(signal.SIGINT, _sigint)

    sys.stdout.write(ANSI_HIDE_CURSOR + ANSI_CLR_SCREEN + ANSI_HOME)
    sys.stdout.flush()

    buf = b""
    last_render = 0.0
    samples = 0
    rate_window_start = time.time()
    hz = 0.0
    latest = None

    try:
        while not stop["flag"]:
            chunk = s.read(512)
            if chunk:
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode(errors="replace").strip()
                    m = LINE_RE.search(text)
                    if m:
                        ax, ay, az, gx, gy, gz = (float(v) for v in m.groups())
                        latest = (ax, ay, az, gx, gy, gz)
                        samples += 1
            now = time.time()
            if now - rate_window_start >= 1.0:
                hz = samples / (now - rate_window_start)
                samples = 0
                rate_window_start = now
            if latest and now - last_render >= 0.05:
                render(*latest, hz)
                last_render = now
    finally:
        try:
            s.reset_input_buffer()
            s.write(b"IMU STREAM OFF\r\n")
            s.flush()
            time.sleep(0.2)
        finally:
            s.close()
        sys.stdout.write(ANSI_SHOW_CURSOR + "\n")
        sys.stdout.flush()

    return 0


if __name__ == "__main__":
    sys.exit(main())
