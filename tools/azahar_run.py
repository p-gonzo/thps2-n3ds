"""Run the staged .3dsx in Azahar and report what the program printed.

Closes any running Azahar, launches the game, follows Azahar's log, and returns
as soon as the emulated process ends (or after --timeout seconds if it keeps
running). Prints the program's own output (svcOutputDebugString, which the 3DS
build routes stdout/stderr to) with timestamps.

    python3 tools/azahar_run.py                 # run, stop waiting once it exits
    python3 tools/azahar_run.py --timeout 120   # give a running game longer
    python3 tools/azahar_run.py --keep          # leave Azahar open afterwards (default: always close)

Needs Azahar's "instant debug log" setting on and a log filter that includes
Debug.Emulated:Debug (both in Azahar's qt-config.ini).
"""
import argparse
import os
import re
import subprocess
import time

HOME = os.path.expanduser("~")
APP = os.path.join(HOME, "Applications/Azahar/azahar-macos-arm64-2126.2/Azahar.app")
SUPPORT = os.path.join(HOME, "Library/Application Support/Azahar")
LOG = os.path.join(SUPPORT, "log/azahar_log.txt")
GAME = os.path.join(SUPPORT, "sdmc/3ds/thps2/thps2.3dsx")

OUT = re.compile(r"^\[\s*([0-9.]+)\] Debug\.Emulated <Debug> \S+: ?(.*)$")
END = re.compile(r"Cleaning up process|<Critical>")
FPS = re.compile(r"\[FPS\] game: ([0-9.]+) fps .*frames: (\d+)")
RANGES = ((20, 70, "BIOS start"), (125, 200, "Sony logo"), (790, 1000, "game start"))


def azahar_pids():
    r = subprocess.run(["pgrep", "-x", "azahar"], capture_output=True, text=True)
    return [int(p) for p in r.stdout.split()]


def close_azahar():
    pids = azahar_pids()
    for p in pids:
        subprocess.run(["kill", str(p)])
    deadline = time.time() + 5
    while azahar_pids() and time.time() < deadline:
        time.sleep(0.2)
    for p in azahar_pids():
        subprocess.run(["kill", "-9", str(p)])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=GAME)
    ap.add_argument("--timeout", type=float, default=60.0, help="seconds to wait while it keeps running")
    ap.add_argument("--keep", action="store_true", help="leave Azahar open at the end")
    ap.add_argument("--quiet", action="store_true", help="print only [FPS] lines and the summary")
    ap.add_argument("--until-frame", type=int, default=0,
                    help="stop once an [FPS] line reports this guest frame (needs PSX_FPS_TELEMETRY=1)")
    a = ap.parse_args()

    close_azahar()
    # LaunchServices needs a moment to notice a quit app; opening it again too
    # soon fails with -600 (procNotFound). Settle, then retry briefly.
    time.sleep(1.0)
    for attempt in range(5):
        started = time.time()
        if subprocess.run(["open", APP, "--args", a.game]).returncode == 0:
            break
        time.sleep(1.0)
    else:
        raise SystemExit("could not launch Azahar")

    # Wait for Azahar to start a fresh log for this launch.
    while time.time() - started < 15:
        if os.path.exists(LOG) and os.path.getmtime(LOG) >= started - 1:
            break
        time.sleep(0.2)

    seen = 0
    ended = False
    fps_lines = []
    while time.time() - started < a.timeout:
        with open(LOG, errors="replace") as f:
            lines = f.read().splitlines()
        for line in lines[seen:]:
            m = OUT.match(line)
            if m and m.group(2).strip():
                text = m.group(2)
                fm = FPS.search(text)
                if fm:
                    fps_lines.append((float(fm.group(1)), int(fm.group(2))))
                if not a.quiet or fm:
                    print(f"{float(m.group(1)):8.3f}  {text}", flush=True)
            if END.search(line):
                print(f"{'':8}  [{line.split('> ', 1)[-1].strip()}]", flush=True)
                ended = True
        seen = len(lines)
        if ended or (a.until_frame and fps_lines and fps_lines[-1][1] >= a.until_frame):
            break
        time.sleep(0.25)

    waited = time.time() - started
    print(f"-- {'process ended' if ended else 'still running'} after {waited:.1f}s of wall time")
    # Median fps over fixed guest-frame windows: comparable across builds.
    for lo, hi, label in RANGES:
        vals = sorted(f for f, fr in fps_lines if lo < fr <= hi)
        if vals:
            print(f"-- fps {label:>14} (frames {lo}-{hi}): {vals[len(vals) // 2]:5.1f}  ({len(vals)} readings)")
    # Always close Azahar when done (ended, reached --until-frame, or timed
    # out); --keep leaves it open for a run meant to be watched.
    if not a.keep:
        close_azahar()


if __name__ == "__main__":
    main()
