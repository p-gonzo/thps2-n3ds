#!/usr/bin/env python3
"""Find the first guest frame where two Mac builds diverge.

Runs each build headless along the scripted path into The Hangar (the same
button presses as tools/hangar_state.py), reads psxrecomp's per-frame
fingerprint ring (debug server `frame_fingerprint`: guest cycle count at the
frame, MMIO fingerprint, RAM/scratchpad write fingerprints where the build
records them) and reports the first frame whose fields differ.

    python3 tools/fp_compare.py build-dbg3ds-ref build-dbg3ds --until 2000
"""
import argparse, json, os, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hangar_state as hs

FIELDS = ("cyc", "mmio", "mc", "wr", "wc", "ws", "sp", "sc", "qc")


def collect(build, until):
    exe = os.path.join(hs.R, build, "Tony_Hawks_Pro_Skater_2_Recompiled")
    memcard = tempfile.mkdtemp(prefix="fpcmp-")
    p, d = hs.launch(exe, memcard, {}, "fp.log")
    frames = {}
    try:
        hs.start_route(d)
        last_fetch = 0
        while True:
            f = d("frame")["frame"]
            # Fetch in slices so the ring never wraps past unread frames.
            if f - last_fetch >= 400 or f >= until:
                r = d("frame_fingerprint", count=1024, frame_lo=last_fetch, frame_hi=f)
                for e in r.get("entries", []):
                    frames[e["frame"]] = e
                last_fetch = f
            if f >= until:
                break
            time.sleep(0.005)
    finally:
        hs.stop(p)
    return frames


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--until", type=int, default=2000)
    a = ap.parse_args()
    fa = collect(a.a, a.until)
    fb = collect(a.b, a.until)
    common = sorted(set(fa) & set(fb))
    print(f"frames recorded: {len(fa)} / {len(fb)}, common {len(common)}")
    for f in common:
        diff = [k for k in FIELDS if fa[f].get(k) != fb[f].get(k)]
        if diff:
            print(f"first divergence at frame {f}: fields {diff}")
            for k in diff:
                print(f"   {k}: {fa[f].get(k)}  vs  {fb[f].get(k)}")
            prev = [g for g in common if g < f][-1:]
            if prev:
                print(f"   (frame {prev[0]} equal; cyc {fa[prev[0]].get('cyc')})")
            return
    print("no divergence in common frames")


if __name__ == "__main__":
    main()
