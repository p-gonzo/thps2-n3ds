#!/usr/bin/env python3
"""Make a PS1 guest save state in The Hangar, for fast correctness runs.

Drives the Mac debug build along the scripted path (tools/n3ds/input-script-
hangar.txt timings), saves slot 1 at --frame, and copies the .pst to out/state/.
The 3DS build loads it at launch with PSX_LOAD_SLOT=1 in psx-env.txt once the
file is staged under the card's save folder (tools/n3ds_stage_sd.py --state).

Guest save states are whole-machine PS1 state, independent of the host build,
unlike Azahar's own states. Speed baselines still come from full scripted runs.

    python3 tools/hangar_state.py [--build build-dbg] [--frame 7000]
"""
import argparse, glob, os, shutil, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from psx_debug import PsxDebug

R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PRESSES = [(1780, "START"), (2260, "START"), (2740, "START"), (3220, "CROSS"),
           (3820, "CROSS"), (4540, "CROSS"), (5260, "CROSS"), (6580, "CROSS")]
MASK = {"START": 0xFFF7, "CROSS": 0xBFFF}
HOLD = 9


def start_route(d):
    """Queue PRESSES as a frame-exact input route in the runtime.

    Polling the frame counter from the host and pressing when it passes a
    target slips by however far the game ran between polls, so two runs of
    the same build could press on different frames."""
    d("input_route_clear")
    for i, (f, b) in enumerate(PRESSES):
        d("input_route_append", frames=HOLD, buttons=MASK[b])
        gap = (PRESSES[i + 1][0] - f - HOLD) if i + 1 < len(PRESSES) else 1
        d("input_route_append", frames=gap, buttons=0xFFFF)
    r = d("input_route_start", port=1, at_frame=PRESSES[0][0])
    if not r.get("ok"):
        sys.exit(f"input route refused: {r}")


def route_done_frame():
    return PRESSES[-1][0] + HOLD + 1


def launch(exe, memcard, extra_env=None, log_name="run.log"):
    # A leftover game still listening on the debug port would answer our
    # commands instead of the new process.
    try:
        PsxDebug(); sys.exit("debug port 4370 is already in use: close the other game first")
    except OSError:
        pass
    env = dict(os.environ, SDL_AUDIO_DRIVER="dummy", PSX_UNPACED="1",
               PSX_INTERNAL_RESOLUTION="native", PSX_SUPERSAMPLE="1.0")
    env.update(extra_env or {})
    p = subprocess.Popen([exe, "--no-launcher", "--hidden-window", "--renderer", "software",
                          "--memcard-dir", memcard,
                          "--bios", R + "/bios/SCPH1001.bin",
                          "--disc", R + "/disc/Tony Hawk's Pro Skater 2 (USA).cue"],
                         cwd=os.path.dirname(exe), env=env,
                         stdout=open(os.path.join(memcard, log_name), "w"),
                         stderr=subprocess.STDOUT)
    for _ in range(120):
        try: return p, PsxDebug()
        except OSError: time.sleep(0.25)
    p.kill(); sys.exit("debug server did not come up")


def stop(p):
    p.terminate()
    try: p.wait(10)
    except subprocess.TimeoutExpired: p.kill()


def wait_op(d, what):
    for _ in range(2000):
        s = d("savestate_status")
        st = s.get("status", s)
        if not st.get("pending"):
            if not st.get("last_ok"): sys.exit(f"{what} failed: {s}")
            return
        time.sleep(0.01)
    sys.exit(f"{what} timed out")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build-dbg")
    ap.add_argument("--frame", type=int, default=7000)
    a = ap.parse_args()
    exe = os.path.join(R, a.build, "Tony_Hawks_Pro_Skater_2_Recompiled")
    memcard = tempfile.mkdtemp(prefix="hangar-state-")

    p, d = launch(exe, memcard)
    try:
        start_route(d)
        while d("frame")["frame"] < max(a.frame, route_done_frame()):
            time.sleep(0.005)
        r = d("savestate", op="save", slot=1)
        if not r.get("ok"): sys.exit(f"save refused: {r}")
        wait_op(d, "save")
        saved_at = d("frame")["frame"]
    finally:
        stop(p)
    pst = glob.glob(os.path.join(memcard, "**", "state_*_slot01.pst"), recursive=True)
    if len(pst) != 1: sys.exit(f"expected one slot-1 state, found {pst}")
    rel = os.path.relpath(pst[0], memcard)
    print(f"saved at frame ~{saved_at}: {rel} ({os.path.getsize(pst[0])} bytes)")

    # Reload check on the Mac: a fresh process with PSX_LOAD_SLOT=1 must show
    # The Hangar a few seconds after launch (booting normally takes ~2 min).
    # Judged from a screenshot: in lean + observation-time builds the
    # savestate_status bookkeeping does not report the boot-time load.
    out = os.path.join(R, "out", "state")
    os.makedirs(out, exist_ok=True)
    p, d = launch(exe, memcard, {"PSX_LOAD_SLOT": "1"}, "reload.log")
    try:
        time.sleep(6)
        d("screenshot", path="reload.png")
        time.sleep(0.5)
        shutil.move(os.path.join(os.path.dirname(exe), "reload.png"),
                    os.path.join(out, "reload.png"))
        print(f"reload screenshot (should be The Hangar): {os.path.join(out, 'reload.png')}"
              f"  status {d('savestate_status')}")
    finally:
        stop(p)

    dst = os.path.join(out, os.path.basename(rel))
    shutil.copy2(pst[0], dst)
    with open(os.path.join(out, "relpath.txt"), "w") as fh:
        fh.write(rel + "\n")
    print("copied to", dst, "| card path under the save folder:", rel)
    shutil.rmtree(memcard, ignore_errors=True)


if __name__ == "__main__":
    main()
