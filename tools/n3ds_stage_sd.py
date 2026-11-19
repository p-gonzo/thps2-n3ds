"""Stage the 3DS build and your own game files onto an SD-card root.

Writes <sd>/3ds/thps2/ with everything the .3dsx needs at run time:

    thps2.3dsx                     the build (from build-3ds/)
    game.toml, game_options.toml   title configuration
    bios/SCPH1001.bin              your BIOS dump
    disc/<name>.cue + .bin         the raw image generate extracted from your disc

    python3 tools/n3ds_stage_sd.py --sd "$HOME/Library/Application Support/Azahar/sdmc"
    python3 tools/n3ds_stage_sd.py --sd /Volumes/NINTENDO3DS

With --state, also stages the guest save state from tools/hangar_state.py
(out/state/) into saves/ and writes PSX_LOAD_SLOT=1 into psx-env.txt, so the
game starts in The Hangar within seconds; --no-state removes both again.

The disc image is about 750 MB; it is copied only when missing or changed.
Nothing here is ever committed: it is your own data on your own card.
"""
import argparse
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DISC_NAME = "Tony Hawk's Pro Skater 2 (USA)"


def copy_if_changed(src, dst):
    if not os.path.exists(src):
        sys.exit(f"missing: {src}")
    if os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(src) \
            and os.path.getmtime(dst) >= os.path.getmtime(src):
        print(f"  up to date  {os.path.relpath(dst, ARGS.sd)}")
        return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    print(f"  copied      {os.path.relpath(dst, ARGS.sd)}")


def main():
    global ARGS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sd", required=True, help="SD-card root (Azahar's sdmc folder or the mounted card)")
    ap.add_argument("--build", default=os.path.join(ROOT, "build-3ds"))
    ap.add_argument("--bios", default=os.path.join(ROOT, "bios", "SCPH1001.bin"))
    ap.add_argument("--disc-dir", default=os.path.join(ROOT, "disc"))
    st = ap.add_mutually_exclusive_group()
    st.add_argument("--state", action="store_true",
                    help="stage out/state/ and load it at launch (PSX_LOAD_SLOT=1)")
    st.add_argument("--no-state", action="store_true",
                    help="stop loading the staged state at launch")
    ARGS = ap.parse_args()

    if not os.path.isdir(ARGS.sd):
        sys.exit(f"SD root does not exist: {ARGS.sd}")
    dest = os.path.join(ARGS.sd, "3ds", "thps2")
    print(f"staging into {dest}")
    copy_if_changed(os.path.join(ARGS.build, "thps2.3dsx"), os.path.join(dest, "thps2.3dsx"))
    for cfg in ("game.toml", "game_options.toml"):
        if os.path.exists(os.path.join(ROOT, cfg)):
            copy_if_changed(os.path.join(ROOT, cfg), os.path.join(dest, cfg))
    copy_if_changed(ARGS.bios, os.path.join(dest, "bios", "SCPH1001.bin"))
    for ext in (".cue", ".bin"):
        copy_if_changed(os.path.join(ARGS.disc_dir, DISC_NAME + ext),
                        os.path.join(dest, "disc", DISC_NAME + ext))
    os.makedirs(os.path.join(dest, "saves"), exist_ok=True)
    # The runtime's remembered BIOS pick (what the desktop launcher writes after
    # its file picker): one line, relative to the program's folder. The 3DS has
    # no picker, so stage it.
    with open(os.path.join(dest, "bios.cfg"), "w") as f:
        f.write("bios/SCPH1001.bin\n")
    print("  wrote       3ds/thps2/bios.cfg")
    if ARGS.state or ARGS.no_state:
        set_load_slot(dest, ARGS.state)


def set_load_slot(dest, on):
    state_dir = os.path.join(ROOT, "out", "state")
    if on:
        rel_file = os.path.join(state_dir, "relpath.txt")
        if not os.path.exists(rel_file):
            sys.exit("no out/state/relpath.txt: run tools/hangar_state.py first")
        rel = open(rel_file).read().strip()
        copy_if_changed(os.path.join(state_dir, os.path.basename(rel)),
                        os.path.join(dest, "saves", rel))
    env = os.path.join(dest, "psx-env.txt")
    lines = open(env).read().splitlines() if os.path.exists(env) else []
    lines = [l for l in lines if not l.startswith("PSX_LOAD_SLOT=")]
    if on: lines.append("PSX_LOAD_SLOT=1")
    with open(env, "w") as f:
        f.write("".join(l + "\n" for l in lines))
    print(f"  {'set' if on else 'cleared'}     PSX_LOAD_SLOT in 3ds/thps2/psx-env.txt")


if __name__ == "__main__":
    main()
