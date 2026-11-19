#!/usr/bin/env python3
"""Phase 0 of the PICA200 renderer plan: measure how THPS2 uses VRAM.

Runs the Mac debug build (software renderer, muted, hidden window, unpaced)
along the scripted path into The Hangar and reads the runtime's `vram_census`
debug-server command over guest-frame windows. Prints one summary per window
and writes the raw JSON next to the captures.

    python3 tools/vram_census.py [--build build-dbg] [--out out/census.json]
"""
import argparse, json, os, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from psx_debug import PsxDebug

R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRESSES = [(1780, "START"), (2260, "START"), (2740, "START"), (3220, "CROSS"),
           (3820, "CROSS"), (4540, "CROSS"), (5260, "CROSS"), (6580, "CROSS")]
MASK = {"START": 0xFFF7, "CROSS": 0xBFFF}
WINDOWS = [(130, 200, "sony logo"), (790, 1000, "game start"),
           (1200, 1700, "intro"), (1800, 3200, "menus"),
           (3300, 6500, "menus+loading"), (7000, 7400, "gameplay"),
           (7400, 9000, "gameplay 2")]
ARM_AT = 100


def summary(name, c, frames):
    p = max(c["presents"], 1)
    per = lambda k: c[k] / p
    print(f"\n== {name}: {c['presents']} presents over {frames} frames")
    print(f"  display: gpu {c['present_gpu']}  cpu {c['present_cpu']}  "
          f"mixed {c['present_mixed']}  depth24 {c['present_depth24']}")
    print(f"  prims/present: gpu {per('prims_gpu'):.0f} (tex {per('prims_gpu_tex'):.0f})"
          f"  cpu {per('prims_cpu'):.0f} (tex {per('prims_cpu_tex'):.0f})")
    print(f"  fills gpu/cpu {c['fills_gpu']}/{c['fills_cpu']}  uploads {c['uploads']}"
          f" (into gpu {c['uploads_into_gpu']}, {c['upload_into_gpu_cells']} cells)")
    print(f"  copies {c['copies']}: gpu->gpu {c['copy_gpu_to_gpu']}  gpu->cpu "
          f"{c['copy_gpu_to_cpu']}  cpu->target {c['copy_cpu_to_gpu_target']}")
    print(f"  texture from gpu: by gpu draw {c['tex_from_gpu_by_gpu']}, by cpu draw "
          f"{c['tex_from_gpu_by_cpu']}; cpu draws into gpu {c['cpu_draw_into_gpu']}")
    print(f"  gpuread {c['gpuread']} (from gpu {c['gpuread_from_gpu']})")
    print(f"  READBACK: {c['readback_events']} events, {c['readback_cells']} cells "
          f"({per('readback_cells'):.1f}/present), frames with readback "
          f"{c['frames_with_readback']}/{c['presents']}")
    print(f"  sync-up (CPU content into GPU target): {c['sync_up_cells']} cells "
          f"({per('sync_up_cells'):.1f}/present), frames {c['frames_with_sync_up']}")
    top = sorted(c["areas"], key=lambda a: -a["prims"])[:6]
    for a in top:
        print(f"    area ({a['x1']},{a['y1']})-({a['x2']},{a['y2']}) prims {a['prims']}"
              f" tex {a['tex']} {'TARGET' if a['target'] else 'offscreen'}")
    for r in sorted(c.get("copy_rects", []), key=lambda r: -r["n"])[:5]:
        print(f"    copy ({r['sx']},{r['sy']})->({r['dx']},{r['dy']}) {r['w']}x{r['h']} x{r['n']}")
    for d in c["displays"]:
        if d["presents"]:
            print(f"    display ({d['x']},{d['y']}) {d['w']}x{d['h']} x{d['presents']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build-dbg")
    ap.add_argument("--out", default="out/census.json")
    a = ap.parse_args()
    exe = os.path.join(R, a.build, "Tony_Hawks_Pro_Skater_2_Recompiled")
    env = dict(os.environ, SDL_AUDIO_DRIVER="dummy", PSX_UNPACED="1",
               PSX_INTERNAL_RESOLUTION="native", PSX_SUPERSAMPLE="1.0")
    p = subprocess.Popen([exe, "--no-launcher", "--hidden-window", "--renderer", "software",
                          "--bios", R + "/bios/SCPH1001.bin",
                          "--disc", R + "/disc/Tony Hawk's Pro Skater 2 (USA).cue"],
                         cwd=os.path.dirname(exe), env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    results = []
    try:
        d = None
        for _ in range(120):
            try: d = PsxDebug(); break
            except OSError: time.sleep(0.25)
        if d is None: sys.exit("debug server did not come up")
        presses, wins, held, armed, cur = list(PRESSES), list(WINDOWS), None, False, None
        while wins or cur:
            f = d("frame")["frame"]
            if not armed and f >= ARM_AT:
                d("vram_census", reset=1); armed = True
            if held and f >= held:
                d("clear_input"); held = None
            if presses and f >= presses[0][0]:
                _, b = presses.pop(0); d("set_input", buttons=MASK[b]); held = f + 9
            if cur and f >= cur[1]:
                c = d("vram_census")["census"]
                summary(cur[2], c, f - cur[3])
                results.append({"window": cur[2], "from": cur[3], "to": f, "census": c})
                cur = None
            if not cur and wins and f >= wins[0][0]:
                lo, hi, name = wins.pop(0)
                d("vram_census", reset=1); cur = (lo, hi, name, f)
            time.sleep(0.005)
    finally:
        p.terminate()
        try: p.wait(10)
        except subprocess.TimeoutExpired: p.kill()
    out = os.path.join(R, a.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    json.dump(results, open(out, "w"), indent=1)
    print("\nwrote", out)


if __name__ == "__main__":
    main()
