"""Uncapped speed of a Mac build configured like the 3DS one - a proxy benchmark.

Runs the game with no wall-clock pacing (PSX_UNPACED=1, PSX_VSYNC=0) but every
frame still emulated, rendered and presented, on the software renderer at
native resolution, muted, and reports median guest fps over the same guest
frame windows tools/azahar_run.py uses, from the runtime's own [FPS] lines.
No debug server is needed, so the build can match the 3DS configuration.

Build the proxy once (debug tools, launcher, netplay, rewind off; the 3DS
diagnostic settings):
    cmake -S . -B build-maclean -G Ninja -DCMAKE_BUILD_TYPE=Release \\
      -DPSX_DEBUG_TOOLS=OFF -DPSX_RECOMP_UI=OFF -DPSX_NETPLAY=OFF -DPSX_REWIND=OFF \\
      -DPSX_ENABLE_VULKAN=OFF -DPSX_DIAG_CAPACITY_SHIFT=6 -DPSX_DISC_DIGEST_CACHE=ON
    cmake --build build-maclean --target psx-runtime

    python3 tools/mac_proxy_bench.py
    python3 tools/mac_proxy_bench.py --until-frame 1000 --sample out/mac-sample.txt --sample-at 800
    python3 tools/mac_proxy_bench.py --env PSX_TEXT_EQUAL_BITS=0     # A/B a runtime switch

The proxy is a guide, not the verdict: the M3 hides costs the ARM11 pays
(byte compares, 64-bit math, cache misses). Confirm wins in Azahar.
"""
import argparse
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FPS = re.compile(r"\[FPS\] game: ([0-9.]+) fps .*frames: (\d+)")
RANGES = ((20, 70, "BIOS start"), (125, 200, "Sony logo"), (790, 1000, "game start"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=os.path.join(ROOT, "build-maclean", "Tony_Hawks_Pro_Skater_2_Recompiled"))
    ap.add_argument("--until-frame", type=int, default=1005)
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--env", action="append", default=[], help="extra KEY=VALUE for the run (repeatable)")
    ap.add_argument("--sample", help="write a macOS `sample` profile here")
    ap.add_argument("--sample-at", type=int, default=800, help="guest frame at which to start sampling")
    ap.add_argument("--headless", action="store_true",
                    help="no window or SDL present: emulation and software rasterising only (the Mac's "
                         "SDL present is vsync-bound and differs from the 3DS's direct presenter)")
    a = ap.parse_args()

    exe = os.path.abspath(a.exe)
    env = dict(os.environ, SDL_AUDIO_DRIVER="dummy", PSX_INTERNAL_RESOLUTION="native",
               PSX_SUPERSAMPLE="1.0", PSX_UNPACED="1", PSX_VSYNC="0", PSX_FPS_TELEMETRY="1")
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v
    os.makedirs(os.path.join(ROOT, "out"), exist_ok=True)
    log_path = os.path.join(ROOT, "out", "mac-proxy-run.log")
    cmd = [exe, "--no-launcher", "--headless" if a.headless else "--hidden-window", "--renderer", "software",
           "--bios", os.path.join(ROOT, "bios", "SCPH1001.bin"),
           "--disc", os.path.join(ROOT, "disc", "Tony Hawk's Pro Skater 2 (USA).cue")]
    with open(log_path, "w") as log:
        proc = subprocess.Popen(cmd, cwd=os.path.dirname(exe), env=env, stdout=log, stderr=subprocess.STDOUT)
    readings, sampled, t0 = [], False, time.time()
    try:
        while time.time() - t0 < a.timeout and proc.poll() is None:
            with open(log_path, errors="replace") as f:
                readings = [(float(m.group(1)), int(m.group(2))) for m in FPS.finditer(f.read())]
            last = readings[-1][1] if readings else 0
            if a.sample and not sampled and last >= a.sample_at:
                subprocess.run(["sample", str(proc.pid), "3", "1", "-mayDie", "-file", a.sample],
                               capture_output=True)
                sampled = True
            if last >= a.until_frame:
                break
            time.sleep(0.25)
    finally:
        proc.terminate()
        proc.wait(10)

    if not readings:
        sys.exit(f"no [FPS] lines; see {log_path}")
    for lo, hi, label in RANGES:
        vals = sorted(f for f, fr in readings if lo < fr <= hi)
        if vals:
            print(f"-- fps {label:>11} (frames {lo}-{hi}): {vals[len(vals) // 2]:7.1f}  ({len(vals)} readings)")
        else:
            print(f"-- fps {label:>11} (frames {lo}-{hi}):       -")
    if a.sample:
        print(f"profile: {a.sample} (sampling stalls the run; its window's fps is lower)")


if __name__ == "__main__":
    main()
