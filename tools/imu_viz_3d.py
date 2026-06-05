#!/usr/bin/env python3
"""3D IMU visualizer for OneCollar Rev 6 bring-up.

Streams IMU data from the bring-up REPL, runs a complementary filter for
orientation, and renders into a rerun viewer: an oriented board frame,
gravity + gyro arrows, and live strip charts of all six axes plus
magnitudes.

Run:
    python3 tools/imu_viz_3d.py

Ctrl-C stops streaming cleanly and leaves the viewer open.
"""

from __future__ import annotations

import argparse
import math
import re
import signal
import sys
import time

import numpy as np
import rerun as rr
import serial

LINE_RE = re.compile(
    r"A\[g\]\s+x=([-+\d.]+)\s+y=([-+\d.]+)\s+z=([-+\d.]+)\s+\|\s+"
    r"G\[dps\]\s+x=([-+\d.]+)\s+y=([-+\d.]+)\s+z=([-+\d.]+)"
)

ACCEL_TRUST = 0.02
BOARD_HALF = [0.040, 0.015, 0.002]   # 80 x 30 x 4 mm (world units = meters)
ARROW_SCALE = 0.06


def quat_mul(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return np.array([
        aw * bw - ax * bx - ay * by - az * bz,
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
    ])


def quat_normalize(q: np.ndarray) -> np.ndarray:
    n = np.linalg.norm(q)
    return q / n if n > 1e-12 else np.array([1.0, 0.0, 0.0, 0.0])


def quat_from_axis_angle(axis: np.ndarray, angle: float) -> np.ndarray:
    half = angle * 0.5
    s = math.sin(half)
    return np.array([math.cos(half), axis[0] * s, axis[1] * s, axis[2] * s])


def quat_rotate(q: np.ndarray, v: np.ndarray) -> np.ndarray:
    w = q[0]
    qv = q[1:]
    return v + 2.0 * np.cross(qv, np.cross(qv, v) + w * v)


def quat_inverse(q: np.ndarray) -> np.ndarray:
    return np.array([q[0], -q[1], -q[2], -q[3]])


def init_from_accel(a: np.ndarray) -> np.ndarray:
    """Quaternion rotating world +Z onto measured accel direction."""
    n = np.linalg.norm(a)
    if n < 1e-3:
        return np.array([1.0, 0.0, 0.0, 0.0])
    g = a / n
    z = np.array([0.0, 0.0, 1.0])
    dot = float(np.clip(z @ g, -1.0, 1.0))
    if dot > 0.9999:
        return np.array([1.0, 0.0, 0.0, 0.0])
    if dot < -0.9999:
        return np.array([0.0, 1.0, 0.0, 0.0])
    axis = np.cross(z, g)
    axis /= np.linalg.norm(axis)
    return quat_from_axis_angle(axis, math.acos(dot))


def update_orientation(
    q: np.ndarray, a: np.ndarray, w_dps: np.ndarray, dt: float
) -> np.ndarray:
    w_rad = w_dps * (math.pi / 180.0)
    omega = float(np.linalg.norm(w_rad))
    if omega * dt > 1e-9:
        axis = w_rad / omega
        q = quat_normalize(quat_mul(q, quat_from_axis_angle(axis, omega * dt)))

    a_mag = float(np.linalg.norm(a))
    if abs(a_mag - 1.0) < 0.3:  # only correct when close to 1 g (not during shakes)
        a_n = a / a_mag
        world_z = np.array([0.0, 0.0, 1.0])
        predicted_body_up = quat_rotate(quat_inverse(q), world_z)
        axis = np.cross(predicted_body_up, a_n)
        n = float(np.linalg.norm(axis))
        if n > 1e-6:
            axis /= n
            angle = math.acos(float(np.clip(predicted_body_up @ a_n, -1.0, 1.0)))
            q = quat_normalize(quat_mul(q, quat_from_axis_angle(axis, angle * ACCEL_TRUST)))
    return q


def setup_scene() -> None:
    rr.log("world", rr.ViewCoordinates.RIGHT_HAND_Z_UP, static=True)
    rr.log(
        "world/axes",
        rr.Arrows3D(
            vectors=[[0.10, 0, 0], [0, 0.10, 0], [0, 0, 0.10]],
            origins=[[0, 0, 0], [0, 0, 0], [0, 0, 0]],
            colors=[[255, 64, 64], [64, 255, 64], [80, 140, 255]],
            labels=["X", "Y", "Z"],
        ),
        static=True,
    )


def log_frame(q: np.ndarray, a: np.ndarray, w_dps: np.ndarray) -> None:
    rr.log(
        "world/body",
        rr.Transform3D(rotation=rr.Quaternion(xyzw=[q[1], q[2], q[3], q[0]])),
    )
    rr.log(
        "world/body/board",
        rr.Boxes3D(half_sizes=[BOARD_HALF], colors=[[80, 160, 255]]),
    )
    rr.log(
        "world/body/axes",
        rr.Arrows3D(
            vectors=[[0.06, 0, 0], [0, 0.04, 0], [0, 0, 0.02]],
            origins=[[0, 0, 0], [0, 0, 0], [0, 0, 0]],
            colors=[[255, 80, 80], [80, 255, 80], [80, 120, 255]],
        ),
    )
    # Measured accel in the body frame (≈ gravity-in-body when at rest)
    rr.log(
        "world/body/accel",
        rr.Arrows3D(
            vectors=[(a * ARROW_SCALE).tolist()],
            origins=[[0, 0, 0]],
            colors=[[255, 220, 60]],
            labels=["a"],
        ),
    )
    # Gyro angular velocity arrow in body frame (scaled by ±500 dps range)
    rr.log(
        "world/body/gyro",
        rr.Arrows3D(
            vectors=[(w_dps / 500.0 * ARROW_SCALE).tolist()],
            origins=[[0, 0, 0]],
            colors=[[255, 80, 200]],
            labels=["ω"],
        ),
    )
    # World-frame gravity reference (always pointing -Z)
    rr.log(
        "world/gravity_ref",
        rr.Arrows3D(
            vectors=[[0, 0, -0.05]],
            origins=[[0, 0, 0.08]],
            colors=[[160, 160, 160]],
        ),
    )

    rr.log("plots/accel/ax", rr.Scalars(float(a[0])))
    rr.log("plots/accel/ay", rr.Scalars(float(a[1])))
    rr.log("plots/accel/az", rr.Scalars(float(a[2])))
    rr.log("plots/accel/|a|", rr.Scalars(float(np.linalg.norm(a))))
    rr.log("plots/gyro/gx", rr.Scalars(float(w_dps[0])))
    rr.log("plots/gyro/gy", rr.Scalars(float(w_dps[1])))
    rr.log("plots/gyro/gz", rr.Scalars(float(w_dps[2])))
    rr.log("plots/gyro/|w|", rr.Scalars(float(np.linalg.norm(w_dps))))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    rr.init("oc.imu.3d", spawn=False)
    grpc_url = rr.serve_grpc(
        grpc_port=9876,
        server_memory_limit="256MiB",
        cors_allow_origin=["*"],
    )
    rr.serve_web_viewer(web_port=9090, open_browser=False, connect_to=grpc_url)
    from urllib.parse import quote
    viewer_url = f"http://localhost:9090/?url={quote(grpc_url, safe='')}"
    print(f"\nOpen this URL in your Windows browser:\n  {viewer_url}\n", flush=True)
    setup_scene()

    s = serial.Serial(args.port, args.baud, timeout=0.2)
    time.sleep(0.1)
    s.reset_input_buffer()
    s.write(b"IMU INIT\r\n")
    time.sleep(0.3)
    s.reset_input_buffer()
    s.write(b"IMU STREAM ON\r\n")

    stop = {"flag": False}

    def _sigint(signum, frame):  # noqa: ARG001
        stop["flag"] = True

    signal.signal(signal.SIGINT, _sigint)

    buf = b""
    q = np.array([1.0, 0.0, 0.0, 0.0])
    initialized = False
    last_t: float | None = None

    try:
        while not stop["flag"]:
            chunk = s.read(512)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                m = LINE_RE.search(line.decode(errors="replace"))
                if not m:
                    continue
                vals = [float(v) for v in m.groups()]
                a = np.array(vals[0:3])
                w_dps = np.array(vals[3:6])

                now = time.time()
                dt = 0.05 if last_t is None else max(1e-3, now - last_t)
                last_t = now

                if not initialized:
                    q = init_from_accel(a)
                    initialized = True
                else:
                    q = update_orientation(q, a, w_dps, dt)

                rr.set_time("t", timestamp=now)
                log_frame(q, a, w_dps)
    finally:
        try:
            s.write(b"IMU STREAM OFF\r\n")
            s.flush()
            time.sleep(0.2)
        finally:
            s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
