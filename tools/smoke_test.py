"""Scripted boot-to-gameplay smoke test for a debug-tools build.

Launches the game in a hidden window, presses through the menus into The
Hangar, and saves timed screenshots. Prints the guest frame number at each
shot: about 60 per second of wall time means full speed.

    python3 tools/smoke_test.py                       # uses build-dbg/ and the repo layout
    python3 tools/smoke_test.py --exe build-dbg/Tony_Hawks_Pro_Skater_2_Recompiled --out out/captures/smoke

Build the exe with:  cmake -S . -B build-dbg -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_DEBUG_TOOLS=ON
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from psx_debug import PsxDebug  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
START, CROSS = 0xFFF7, 0xBFFF  # active-low PS1 pad masks

# (seconds after launch, action): intro -> title -> career -> skater -> level -> play
PLAN = [(8, "shot"), (16, "shot"), (24, "shot"), (30, "start"), (34, "shot"), (38, "start"),
        (42, "shot"), (46, "start"), (50, "shot"), (54, "cross"), (60, "shot"), (64, "cross"),
        (72, "shot"), (76, "cross"), (86, "shot"), (88, "cross"), (100, "shot"), (108, "shot"),
        (110, "cross"), (116, "shot"), (122, "shot"), (130, "shot")]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=os.path.join(ROOT, "build-dbg", "Tony_Hawks_Pro_Skater_2_Recompiled"))
    ap.add_argument("--bios", default=os.path.join(ROOT, "bios", "SCPH1001.bin"))
    ap.add_argument("--disc", default=os.path.join(ROOT, "disc", "Tony Hawk's Pro Skater 2 (USA).cue"))
    ap.add_argument("--out", default=os.path.join(ROOT, "out", "captures", "smoke"))
    a = ap.parse_args()

    exe = os.path.abspath(a.exe)
    exe_dir = os.path.dirname(exe)
    os.makedirs(a.out, exist_ok=True)
    log = open(os.path.join(a.out, "run.log"), "w")
    proc = subprocess.Popen([exe, "--no-launcher", "--hidden-window", "--bios", os.path.abspath(a.bios),
                             "--disc", os.path.abspath(a.disc)], cwd=exe_dir, stdout=log, stderr=subprocess.STDOUT)
    try:
        dbg = None
        for _ in range(60):
            try:
                dbg = PsxDebug()
                break
            except OSError:
                if proc.poll() is not None:
                    sys.exit(f"game exited early; see {log.name}")
                time.sleep(0.5)
        if dbg is None:
            sys.exit("debug server never came up (was the exe built with PSX_DEBUG_TOOLS=ON?)")

        t0, n = time.time(), 0
        for at, act in PLAN:
            while time.time() - t0 < at:
                time.sleep(0.1)
            if act == "shot":
                n += 1
                name = f"shot_{n:02d}_{at:03d}s.png"  # the server only accepts paths relative to the exe
                frame = dbg("frame").get("frame")
                dbg("screenshot", path=name)
                shutil.move(os.path.join(exe_dir, name), os.path.join(a.out, name))
                print(f"{at:4d}s  frame {frame:>6}  {name}", flush=True)
            else:
                dbg("set_input", buttons=START if act == "start" else CROSS)
                time.sleep(0.15)
                dbg("clear_input")
                print(f"{at:4d}s  press {act}", flush=True)
        print("still running at the end:", proc.poll() is None)
    finally:
        proc.terminate()
        proc.wait(10)


if __name__ == "__main__":
    main()
