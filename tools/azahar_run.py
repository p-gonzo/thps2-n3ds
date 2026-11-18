"""Run the staged .3dsx in Azahar and report what the program printed.

Closes any running Azahar, launches the game, follows Azahar's log, and returns
as soon as the emulated process ends (or after --timeout seconds if it keeps
running). Prints the program's own output (svcOutputDebugString, which the 3DS
build routes stdout/stderr to) with timestamps.

    python3 tools/azahar_run.py                 # run, stop waiting once it exits
    python3 tools/azahar_run.py --timeout 120   # give a running game longer
    python3 tools/azahar_run.py --keep          # leave Azahar open afterwards

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
    while time.time() - started < a.timeout:
        with open(LOG, errors="replace") as f:
            lines = f.read().splitlines()
        for line in lines[seen:]:
            m = OUT.match(line)
            if m and m.group(2).strip():
                print(f"{float(m.group(1)):8.3f}  {m.group(2)}", flush=True)
            if END.search(line):
                print(f"{'':8}  [{line.split('> ', 1)[-1].strip()}]", flush=True)
                ended = True
        seen = len(lines)
        if ended:
            break
        time.sleep(0.25)

    waited = time.time() - started
    print(f"-- {'process ended' if ended else 'still running'} after {waited:.1f}s of wall time")
    # A program that ended leaves Azahar on its game list: close it. One that
    # is still running stays open so it can be watched.
    if ended and not a.keep:
        close_azahar()


if __name__ == "__main__":
    main()
