"""Uncapped speed of a Mac build configured like the 3DS one - a proxy benchmark.

Runs the game with turbo on (no frame pacing), the software renderer at native
resolution and muted audio, and reports guest frames per second over fixed
guest-frame ranges, so numbers compare across builds and against the 3DS. With
--sample, also records a macOS `sample` profile while the Sony logo draws.

Build the proxy once (the 3DS diagnostic settings, plus the debug server):
    cmake -S . -B build-mac3ds -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DPSX_DEBUG_TOOLS=ON -DPSX_DIAG_CAPACITY_SHIFT=6 -DPSX_DISC_DIGEST_CACHE=ON
    cmake --build build-mac3ds --target psx-runtime

    python3 tools/mac_proxy_bench.py
    python3 tools/mac_proxy_bench.py --sample out/mac-sample.txt

Measured 2026-11-18 (M3 Pro vs Azahar New 3DS): Sony logo, guest frames
120-200, 162 fps on the Mac proxy and 2.0 fps on the 3DS, about 80x.
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from psx_debug import PsxDebug  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RANGES = ((120, 200), (300, 600), (600, 1500))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=os.path.join(ROOT, "build-mac3ds", "Tony_Hawks_Pro_Skater_2_Recompiled"))
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--sample", help="write a macOS sample profile of the logo frames here")
    a = ap.parse_args()

    exe = os.path.abspath(a.exe)
    env = dict(os.environ, SDL_AUDIO_DRIVER="dummy", PSX_INTERNAL_RESOLUTION="native", PSX_SUPERSAMPLE="1.0")
    cmd = [exe, "--no-launcher", "--hidden-window", "--renderer", "software",
           "--bios", os.path.join(ROOT, "bios", "SCPH1001.bin"),
           "--disc", os.path.join(ROOT, "disc", "Tony Hawk's Pro Skater 2 (USA).cue")]
    log = open(os.path.join(ROOT, "out", "mac-proxy-run.log"), "w") if os.path.isdir(os.path.join(ROOT, "out")) else subprocess.DEVNULL
    proc = subprocess.Popen(cmd, cwd=os.path.dirname(exe), env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        dbg = None
        for _ in range(80):
            try:
                dbg = PsxDebug()
                break
            except OSError:
                time.sleep(0.25)
        if dbg is None:
            sys.exit("debug server never came up (build with -DPSX_DEBUG_TOOLS=ON)")
        dbg("turbo", enabled=1)
        samples, t0 = [], time.time()
        sampled = False
        while time.time() - t0 < a.seconds:
            frame = dbg("frame").get("frame", 0)
            samples.append((time.time() - t0, frame))
            if a.sample and not sampled and frame >= 125:
                subprocess.run(["sample", str(proc.pid), "2", "1", "-mayDie", "-file", a.sample],
                               capture_output=True)
                sampled = True
                samples.clear()   # the profiler stalls the process; do not time across it
            time.sleep(0.05)
    finally:
        proc.terminate()
        proc.wait(10)

    for lo, hi in RANGES:
        pts = [(t, f) for t, f in samples if lo <= f <= hi]
        if len(pts) >= 2 and pts[-1][0] > pts[0][0]:
            print(f"guest frames {lo:4d}-{hi:4d}: {(pts[-1][1] - pts[0][1]) / (pts[-1][0] - pts[0][0]):7.0f} fps")
        else:
            print(f"guest frames {lo:4d}-{hi:4d}:       - (not reached in this window)")
    if a.sample:
        print(f"profile: {a.sample}")


if __name__ == "__main__":
    main()
