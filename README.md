# THPS2 for New Nintendo 3DS

A work-in-progress port of the PlayStation version of **Tony Hawk's Pro Skater 2**
(USA, SLUS-01066) to the **New Nintendo 3DS**, aiming for full speed and a
60 fps presentation.

It is built on a *static recompilation* of the game: the PS1 MIPS code is
translated to C ahead of time by [psxrecomp](https://github.com/RetroPortingToolKit/psxrecomp)
and linked against a runtime that stands in for the PlayStation hardware. This
repository starts from Alexbeav's desktop recompilation,
[tony-hawk-s-pro-skater-2-recomp](https://github.com/Alexbeav/tony-hawk-s-pro-skater-2-recomp),
and adds the 3DS port on top.

Nothing from the game ships here. You need your own disc image and your own
PlayStation BIOS dump.

## Status

| Milestone | State |
|---|---|
| macOS build plays through menus into a level at full speed | **done** |
| 3DS toolchain set up; feasibility check of the code on ARM | **done**: 278 of 290 source files compile unchanged |
| 3DS build compiles, links and fits in New 3DS memory | **done** |
| 3DS build boots the PlayStation BIOS (Sony logo) and reaches the game's code (Activision logo) | **done** in Azahar, at about 2 fps; not yet tried on hardware |
| Speed: full speed on New 3DS | **next** |
| In-game on 3DS (menus, input, gameplay) | planned |
| Audio, citro3d GPU renderer | planned |
| Widescreen, stereoscopic 3D, bottom-screen UI, 60 fps | planned |
| CIA packaging | planned |

There is no 3DS build to download yet. The plan, the findings so far and the
technical constraints are in [docs/N3DS_PORT_PLAN.md](docs/N3DS_PORT_PLAN.md).

## What you need

**Game files (yours, never committed):**

- A dump of the USA disc, SLUS-01066. Either a `.chd`, or the raw
  `.bin`/`.cue`. The data track must match the Redump dump
  (MD5 `ac85ae22723eaf944b0f899aa53244dd`). The build checks this.
- The SCPH-1001 BIOS, 512 KB, SHA-256
  `71af94d1e47a68c11e8fdb9f8368040601514a42a5a399cda48c7d3bff1e99d3`.
  Other BIOS revisions and OpenBIOS are not accepted.

Put them here; all of these folders are gitignored:

```
bios/SCPH1001.bin
disk/<your image>.chd        (or a .bin/.cue pair)
```

**macOS tools:**

- Xcode Command Line Tools (`xcode-select --install`)
- Python 3.9 or newer
- CMake 3.20 or newer and Ninja. With Homebrew: `brew install cmake ninja`.
  Without it: `python3 -m pip install cmake ninja`.

SDL3 is built from a copy vendored in psxrecomp; nothing else needs installing.

**For the 3DS work:** [devkitPro](https://devkitpro.org/wiki/Getting_Started)
with the `3ds-dev` group and `3ds-zlib`
(`sudo dkp-pacman -S 3ds-dev 3ds-zlib`).

## Building for macOS

The desktop build is the reference every 3DS result is checked against.

```bash
git clone --recursive https://github.com/p-gonzo/thps2-n3ds.git
cd thps2-n3ds
```

Build the recompiler, then generate the game's C from your disc. Generation
verifies the disc, extracts a raw `.bin`/`.cue` and the game executable into
`disc/`, and writes about 8 million lines of C into `generated/`:

```bash
bash psxrecomp/tools/ci/build_emitters.sh
```

```bash
python3 psxrecomp/psxrecomp_cli.py generate --config game.toml --project-root . --disc disk/<your image>.chd --bios bios/SCPH1001.bin
```

Build and run:

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build-release --target psx-runtime
```

```bash
./build-release/Tony_Hawks_Pro_Skater_2_Recompiled
```

If the launcher window appears, `--no-launcher` skips it and boots straight
into the game.

### Automated smoke test

A build with the debug server enabled can be driven over TCP (port 4370):

```bash
cmake -S . -B build-dbg -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_DEBUG_TOOLS=ON
```

```bash
cmake --build build-dbg --target psx-runtime
```

```bash
python3 tools/smoke_test.py
```

It boots the game in a hidden window, presses through the menus into The
Hangar, and saves timed screenshots to `out/captures/smoke/`. It prints the
game's frame number at each shot; about 60 per second means full speed.
`tools/psx_debug.py` sends single commands to a running game.

## Building for New 3DS

Needs devkitPro with `3ds-dev` and `3ds-zlib`, and `generated/` from the macOS
steps above (code generation always runs on a PC).

```bash
cmake -S . -B build-3ds -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release
```

```bash
ninja -C build-3ds thps2.3dsx
```

The 3DS build turns off the desktop launcher, Vulkan, netplay, rewind and the
debug server on its own. Stage it with your BIOS and disc onto an SD-card
root (the emulator's or your card's):

```bash
python3 tools/n3ds_stage_sd.py --sd "$HOME/Library/Application Support/Azahar/sdmc"
```

That writes `3ds/thps2/` with the `.3dsx`, `game.toml`, `bios/`, `disc/` and a
`bios.cfg` naming the BIOS. The first launch hashes the whole disc image once
(about 5 minutes on 3DS hardware speed) and caches the result next to it;
later launches skip it.

### Testing in Azahar

`tools/azahar_run.py` closes any running Azahar, starts the staged game,
prints the program's output as it runs, and returns as soon as it exits.
Azahar needs, in its `qt-config.ini`: `instant_debug_log=true` and
`log_filter=*:Info Debug.Emulated:Debug` (the 3DS build routes stdout and
stderr to the debug log). An empty file named `wait-for-debugger` in
`3ds/thps2/` holds the program for 15 s at start so GDB
(`arm-none-eabi-gdb` against `build-3ds/*.elf`, Azahar's GDB stub) can
attach.

## Repository layout

| Path | What it is |
|---|---|
| `psxrecomp/` | Submodule: the recompiler and PS1 runtime. Fork [p-gonzo/psxrecomp](https://github.com/p-gonzo/psxrecomp), branch `n3ds`, tracking RetroPortingToolKit. 3DS changes to the framework go there. |
| `recomp-ui/` | Submodule: the desktop launcher and menus ([RetroPortingToolKit/recomp-ui](https://github.com/RetroPortingToolKit/recomp-ui)). Not used on 3DS. |
| `game.toml`, `seeds/`, `symbols.toml` | The title's configuration for psxrecomp. |
| `tools/` | Smoke test, debug client, symbol sync. |
| `docs/N3DS_PORT_PLAN.md` | The port plan. |
| `generated/`, `disc/`, `build-*/`, `out/` | Build output and extracted data. Gitignored; never commit them. |

Branches: port work lands on `main`. Alexbeav's desktop repository is the
`upstream` remote, for pulling title updates (`git fetch upstream`).

To pull framework updates, from inside `psxrecomp/`:

```bash
git fetch rptk && git merge rptk/master
```

Push the psxrecomp fork's branch **before** committing the new submodule
version here, or fresh clones will point at a commit that does not exist.

## Credits

- **Alexbeav**: the THPS2 recompilation this project starts from.
- **psxrecomp** by mstan and the [RetroPortingToolKit](https://github.com/RetroPortingToolKit)
  team: the recompiler and PS1 runtime.
- **CTR-3D** by [zando95](https://github.com/zando95/CTR-3D) and
  [tinaut1986](https://github.com/tinaut1986/CTR-3D): the reference for the
  3DS platform layer (threads, audio, rendering rules, packaging). Their port
  is built on a decompilation, so only those platform patterns carry over.

## Legal

This is a non-commercial fan project, not affiliated with or endorsed by
Activision, Neversoft, Sony or Nintendo. Tony Hawk's Pro Skater is a trademark
of Activision. You must own the original game. Disc images, BIOS dumps,
generated code and anything extracted from the disc are gitignored and must
never be committed or distributed.

Project-owned files are licensed under `PolyForm Noncommercial 1.0.0` (see
[LICENSE](LICENSE)). psxrecomp, recomp-ui and the other dependencies keep their
own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
