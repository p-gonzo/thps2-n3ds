"""Sampling profiler for the 3DS build: in Azahar, or on a real console.

Attaches arm-none-eabi-gdb to Azahar's GDB stub N times, records a backtrace
each time, and reports where the program was: "self" = the innermost frame,
"inclusive" = anywhere in the first --depth frames. Each attach pauses the
game for a moment, so this measures where time goes, not how fast it runs.

Start the game first (tools/azahar_run.py --timeout 600 leaves it running),
with use_gdbstub=true in Azahar's qt-config.ini.

    python3 tools/azahar_profile.py --samples 40
    python3 tools/azahar_profile.py --samples 60 --depth 12 --top 30

On a New 3DS with Luma3DS: start the game from the Homebrew Launcher, open
the Rosalina menu (L + Down + Select), Debugger options -> Enable debugger,
note the console's IP address (Rosalina's main menu or System Settings), and:

    python3 tools/azahar_profile.py --host 192.168.1.50 --samples 60 --classify

That connects to Rosalina's GDB server (port 4000), finds the homebrew
process (named 3dsx_app), and attaches for each sample. build-3ds/*.elf must
be the build that is running on the console. Each sample pauses the game for
a moment over Wi-Fi, so expect about one sample a second.
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


# Same grouping as the Mac ceiling test (docs/N3DS_PORT_PLAN.md).
CATEGORIES = [
    ("guest code (game/BIOS compiled)", r"^(func_[0-9A-F]{8}|SCPH1001_func_|run_compiled_game_entry|psx_game_)"),
    ("GPU raster + display", r"^(raster_|put_|texel_|sw_|gr_|gpu_|gp0_|blend|plot_|draw_|n3ds_present|Blit|SDL_)"),
    ("SPU audio", r"^(spu_|adpcm|reverb|audio_|xa_|cd_audio|N3DSAUD|ndsp)"),
    ("MDEC / CD / DMA data", r"^(mdec|decode_rle|idct|advance_mdec|cdrom_|iso_|chd|dma_|memcpy|memmove|memset|__aeabi_mem)"),
    ("GTE (geometry coprocessor)", r"^(gte_|GTE|psx_gte|rtp|mvmva|nclip)"),
    ("interrupts + timing", r"^(psx_check_interrupts|interrupts_|psx_devices_|psx_advance_cycles|psx_cyc|psx_cpu_charge|psx_native_span|psx_get_cycle_count|timer|timers_|fire_vblank|advance_devices|psx_idle|cdrom_cycles|dma_cycles|psx_rfe)"),
    ("dispatch + code validity", r"^(psx_dispatch|dirty_ram_|psx_static|static_code|psx_unknown_dispatch|text_|overlay_|mod_runtime_on_dispatch|text_xlate|fntrace|psx_segment|try_execute|interp_)"),
    ("store/load bookkeeping", r"^(psx_(write|read)_|cpu_store|psx_phys|psx_memory_note|card_|debug_server|parity|history_|wtrace|d44|psx_native_(store|load))"),
    ("waiting (sync/vblank/svc)", r"^(svc|LightLock|LightEvent|__sync|gspWaitForVBlank|_Z.*wait|__libc_lock|pthread_)"),
    ("64-bit / libgcc helpers", r"^(__aeabi_|__udiv|__div|__mod|__umod)"),
]


def category(fn):
    for name, rx in CATEGORIES:
        if re.match(rx, fn):
            return name
    return "other: " + fn


def find_console_pid(host, port):
    """The PID of the homebrew process on a console running Rosalina's GDB
    server: Luma's homebrew loader names it 3dsx_app."""
    with tempfile.NamedTemporaryFile("w", suffix=".gdb", delete=False) as f:
        f.write("set pagination off\nset confirm off\n"
                f"target extended-remote {host}:{port}\ninfo os processes\ndisconnect\n")
        name = f.name
    try:
        out = subprocess.run([GDB, "-q", "-batch", "-x", name], capture_output=True,
                             text=True, timeout=60)
    finally:
        os.unlink(name)
    for line in out.stdout.splitlines():
        cols = line.split()
        if len(cols) >= 2 and cols[0].isdigit() and "3dsx_app" in line:
            return int(cols[0])
    raise SystemExit("no 3dsx_app process found; is the game running and the Rosalina "
                     "debugger enabled?\n" + out.stdout[-1500:] + out.stderr[-800:])


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
    ap.add_argument("--port", type=int, default=None,
                    help="GDB port (default: 24689 for Azahar, 4000 with --host)")
    ap.add_argument("--host", default=None,
                    help="a console's IP: use Rosalina's GDB server instead of Azahar's stub")
    ap.add_argument("--pid", type=int, default=None,
                    help="process to attach to on the console (default: find 3dsx_app)")
    ap.add_argument("--gap", type=float, default=0.5, help="seconds between samples")
    ap.add_argument("--raw", help="also write every sampled stack here (one per block)")
    ap.add_argument("--classify", action="store_true",
                    help="group innermost frames into runtime cost categories")
    a = ap.parse_args()

    if a.host:
        port = a.port or 4000
        pid = a.pid or find_console_pid(a.host, port)
        target = (f"target extended-remote {a.host}:{port}\n"
                  f"attach {pid}\n")
        print(f"console {a.host}:{port}, process {pid}")
    else:
        target = f"target remote 127.0.0.1:{a.port or 24689}\n"
    with tempfile.NamedTemporaryFile("w", suffix=".gdb", delete=False) as f:
        f.write("set pagination off\nset confirm off\n" + target +
                f"bt {a.depth}\ndetach\n")
        cmdfile = f.name
    self_c, incl_c, n = collections.Counter(), collections.Counter(), 0
    raw = open(a.raw, "w") if a.raw else None
    try:
        for _ in range(a.samples):
            frames = sample(cmdfile)
            if raw and frames:
                raw.write("\n".join(frames) + "\n--\n")
                raw.flush()
            if frames:
                n += 1
                self_c[frames[0]] += 1
                for fn in set(frames):
                    incl_c[fn] += 1
            time.sleep(a.gap)
    finally:
        os.unlink(cmdfile)
    if raw:
        raw.close()
    if not n:
        raise SystemExit("no samples: is the game running in Azahar with the GDB stub on?")
    if a.classify:
        cats = collections.Counter()
        for fn, k in self_c.items():
            cats[category(fn)] += k
        print(f"\ncategories (innermost frame, {n} samples):")
        for name, k in cats.most_common():
            print(f"  {100 * k / n:5.1f}%  {name}")
    print(f"{n} samples")
    for title, c in (("self (innermost frame)", self_c), (f"inclusive (top {a.depth} frames)", incl_c)):
        print(f"\n{title}:")
        for fn, k in c.most_common(a.top):
            print(f"  {100 * k / n:5.1f}%  {fn}")


if __name__ == "__main__":
    main()
