"""Sampling profiler for the 3DS build running in Azahar.

Attaches arm-none-eabi-gdb to Azahar's GDB stub N times, records a backtrace
each time, and reports where the program was: "self" = the innermost frame,
"inclusive" = anywhere in the first --depth frames. Each attach pauses the
game for a moment, so this measures where time goes, not how fast it runs.

Start the game first (tools/azahar_run.py --timeout 600 leaves it running),
with use_gdbstub=true in Azahar's qt-config.ini.

    python3 tools/azahar_profile.py --samples 40
    python3 tools/azahar_profile.py --samples 60 --depth 12 --top 30
"""
import argparse
import collections
import os
import re
import subprocess
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GDB = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-gdb"
ELF = os.path.join(ROOT, "build-3ds", "Tony_Hawks_Pro_Skater_2_Recompiled.elf")
FRAME = re.compile(r"^#(\d+)\s+(?:0x[0-9a-f]+ in )?(\S+?)\s*\(")


def sample(cmdfile):
    out = subprocess.run([GDB, "-q", "-batch", "-x", cmdfile, ELF],
                         capture_output=True, text=True, timeout=60).stdout
    frames = []
    for line in out.splitlines():
        m = FRAME.match(line)
        if m:
            frames.append(m.group(2))
    return frames


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--samples", type=int, default=40)
    ap.add_argument("--depth", type=int, default=10)
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--port", type=int, default=24689)
    ap.add_argument("--gap", type=float, default=0.5, help="seconds between samples")
    a = ap.parse_args()

    with tempfile.NamedTemporaryFile("w", suffix=".gdb", delete=False) as f:
        f.write("set pagination off\nset confirm off\n"
                f"target remote 127.0.0.1:{a.port}\nbt {a.depth}\ndetach\n")
        cmdfile = f.name
    self_c, incl_c, n = collections.Counter(), collections.Counter(), 0
    try:
        for _ in range(a.samples):
            frames = sample(cmdfile)
            if frames:
                n += 1
                self_c[frames[0]] += 1
                for fn in set(frames):
                    incl_c[fn] += 1
            time.sleep(a.gap)
    finally:
        os.unlink(cmdfile)
    if not n:
        raise SystemExit("no samples: is the game running in Azahar with the GDB stub on?")
    print(f"{n} samples")
    for title, c in (("self (innermost frame)", self_c), (f"inclusive (top {a.depth} frames)", incl_c)):
        print(f"\n{title}:")
        for fn, k in c.most_common(a.top):
            print(f"  {100 * k / n:5.1f}%  {fn}")


if __name__ == "__main__":
    main()
