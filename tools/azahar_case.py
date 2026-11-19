#!/usr/bin/env python3
"""Run one renderer test case in Azahar and compare the GPU's frames.

Stages the current build-3ds/thps2.3dsx, writes psx-env.txt for the case,
runs Azahar unthrottled, collects snap_/screen_/gpu_<frame>.bmp into
out/cases/<name>/, and compares every gpu_<frame>.bmp (the 3DS GPU's copy of
the displayed buffer) with snap_<frame>.bmp (the software renderer's frame)
in 5-bit channels, the PS1's colour depth. Afterwards the card's psx-env.txt
is put back to normal (no state, no switches).

    # gameplay from the Hangar save state (fast)
    python3 tools/azahar_case.py hangar --state --env THPS2_GPU_SHADOW=1 --frames 300,600
    # scripted boot through the menus into gameplay
    python3 tools/azahar_case.py menus --script --env THPS2_GPU_SHADOW=1 \\
        --frames 1900,4200,7200 --until 7210

Exit status 1 if a compared frame has pixels more than --budget steps apart
(default 2: gouraud rounding differs by 1-2 steps between the renderers).
"""
import argparse, collections, glob, os, shutil, struct, subprocess, sys

R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CARD = os.path.expanduser("~/Library/Application Support/Azahar/sdmc/3ds/thps2")
SD = os.path.dirname(os.path.dirname(CARD))
BASE_ENV = "# PSX_FPS_TELEMETRY is turned on automatically when logging\n"


def bmp(path):
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    row = (w * 3 + 3) & ~3
    px = [[(d[off + (h - 1 - y) * row + x * 3 + 2] >> 3, d[off + (h - 1 - y) * row + x * 3 + 1] >> 3,
            d[off + (h - 1 - y) * row + x * 3] >> 3) for x in range(w)] for y in range(h)]
    return w, h, px


def compare(gpu, cpu):
    """Histogram of the max per-channel difference in 5-bit steps."""
    w, h, a = bmp(gpu)
    w2, h2, b = bmp(cpu)
    if (w, h) != (w2, h2):
        return None
    hist = collections.Counter()
    for y in range(h):
        for x in range(w):
            hist[max(abs(p - q) for p, q in zip(a[y][x], b[y][x]))] += 1
    return dict(sorted(hist.items()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name")
    ap.add_argument("--state", action="store_true", help="start from the Hangar save state")
    ap.add_argument("--script", action="store_true", help="use tools/n3ds/input-script-hangar.txt")
    ap.add_argument("--env", action="append", default=[], help="KEY=VALUE for psx-env.txt")
    ap.add_argument("--frames", required=True, help="snapshot frames, comma separated")
    ap.add_argument("--until", type=int, default=0, help="stop at this guest frame")
    ap.add_argument("--timeout", type=float, default=90)
    ap.add_argument("--budget", type=int, default=2)
    a = ap.parse_args()

    out = os.path.join(R, "out", "cases", a.name)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    subprocess.run([sys.executable, os.path.join(R, "tools", "n3ds_stage_sd.py"), "--sd", SD]
                   + (["--state"] if a.state else ["--no-state"]), check=True, stdout=subprocess.DEVNULL)
    env_lines = open(os.path.join(CARD, "psx-env.txt")).read() if os.path.exists(os.path.join(CARD, "psx-env.txt")) else BASE_ENV
    with open(os.path.join(CARD, "psx-env.txt"), "a") as f:
        for e in a.env + [f"THPS2_SNAPSHOTS={a.frames}"]:
            f.write(e + "\n")
    if a.script:
        shutil.copy(os.path.join(R, "tools", "n3ds", "input-script-hangar.txt"), os.path.join(CARD, "input-script.txt"))
    for f in glob.glob(os.path.join(CARD, "*_*.bmp")):
        if os.path.basename(f).split("_")[0] in ("snap", "screen", "gpu"):
            os.remove(f)
    try:
        cmd = [sys.executable, os.path.join(R, "tools", "azahar_run.py"), "--unthrottled", "--quiet",
               "--timeout", str(a.timeout)]
        if a.until:
            cmd += ["--until-frame", str(a.until)]
        log = subprocess.run(cmd, capture_output=True, text=True).stdout
        open(os.path.join(out, "run.txt"), "w").write(log)
    finally:
        if a.script and os.path.exists(os.path.join(CARD, "input-script.txt")):
            os.remove(os.path.join(CARD, "input-script.txt"))
        open(os.path.join(CARD, "psx-env.txt"), "w").write(BASE_ENV)
        subprocess.run([sys.executable, os.path.join(R, "tools", "n3ds_stage_sd.py"), "--sd", SD, "--no-state"],
                       stdout=subprocess.DEVNULL)
    for f in glob.glob(os.path.join(CARD, "*_*.bmp")):
        if os.path.basename(f).split("_")[0] in ("snap", "screen", "gpu"):
            shutil.move(f, os.path.join(out, os.path.basename(f)))
    azlog = os.path.expanduser("~/Library/Application Support/Azahar/log/azahar_log.txt")
    lines = [l.split("OutputDebugString:", 1)[-1].split(": ", 1)[-1].rstrip()
             for l in open(azlog, errors="replace") if "OutputDebugString" in l]
    open(os.path.join(out, "program.txt"), "w").write("\n".join(lines) + "\n")
    for l in lines:
        if any(k in l for k in ("FAILED", "GPU ", "shadow renderer", "texcache", "error", "Error")):
            print("  " + l)
    worst = 0
    for f in sorted(glob.glob(os.path.join(out, "gpu_*.bmp")), key=lambda p: int(p.split("_")[-1][:-4])):
        frame = os.path.basename(f)[4:-4]
        snap = os.path.join(out, f"snap_{frame}.bmp")
        if not os.path.exists(snap):
            print(f"frame {frame}: no snap to compare")
            continue
        hist = compare(f, snap)
        if hist is None:
            print(f"frame {frame}: size differs")
            worst = 99
            continue
        total = sum(hist.values())
        over = sum(v for k, v in hist.items() if k > a.budget)
        worst = max(worst, max(hist))
        print(f"frame {frame}: exact {100 * hist.get(0, 0) / total:5.1f}%  1 step {hist.get(1, 0):6d}  "
              f"2 steps {hist.get(2, 0):5d}  over budget {over:5d}  max {max(hist)}")
    print("output:", out)
    sys.exit(1 if worst > a.budget else 0)


if __name__ == "__main__":
    main()
