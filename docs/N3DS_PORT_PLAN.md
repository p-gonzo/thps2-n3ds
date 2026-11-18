# New 3DS port plan (THPS2 on psxrecomp)

Status: planning. Nothing 3DS-specific is built yet.

## Framework base

The upstream title pins Alexbeav's `pin/pin-i-*` psxrecomp branch, which split
from RetroPortingToolKit/psxrecomp `master` on 2026-08-28. This branch instead
tracks **RetroPortingToolKit**, through the fork `p-gonzo/psxrecomp` (branch
`n3ds`), with `recomp-ui` on RetroPortingToolKit `master`.

Checked on macOS before switching (2026-11-18), with the same scripted run
(BIOS → intro → menus → skating The Hangar):

| | Alexbeav `578bd3b` | RetroPortingToolKit `da9be04` |
|---|---|---|
| Reaches gameplay at full speed | yes | yes |
| Generated C | 93 shards, 3.5M lines | 217 shards, 7.9M lines |
| Compiled `__text` (debug-tools build) | 48.7 MB | 32.0 MB |

Remotes inside `psxrecomp/`: `origin` = the fork, `rptk` = RetroPortingToolKit,
`alexbeav` = Alexbeav's fork. Push the fork's `n3ds` branch before committing a
new gitlink here, or fresh clones break.

## The key difference from CTR-3D

CTR-3D sits on a **decompilation** (CTR-ModSDK): the game is readable C, so its
3DS renderer can bake track meshes and draw models natively on the PICA200.

This repo is a **static recompilation**: the game is machine-translated MIPS C
that drives an emulated PS1 hardware runtime (GPU/SPU/CD-ROM/timers/IRQs, plus a
recompiled SCPH-1001 BIOS). So the 3DS work goes into **`psxrecomp/runtime`**,
not into the game code. We reuse CTR-3D's *platform* patterns, not its renderer
design.

What carries over from CTR-3D:

- devkitPro / libctru / citro3d CMake toolchain (`$DEVKITPRO/cmake/3DS.cmake`),
  `build3ds.sh`, CIA packaging (makerom + bannertool, .rsf, SMDH/banner).
- One libctru-facing backend TU kept apart from the game TU, talking through
  plain scalars (enum-ABI split: `-fshort-enums` only on the libctru side).
- Threads: game on core 0, render worker on core 2, audio worker on core 3,
  never touch core 1.
- ndsp audio (DSP firmware dump required), data under `sdmc:/3ds/thps2/`,
  disc image supplied by the player, nothing of the game shipped.
- Bottom-screen UI (status/options/FPS) as one CPU-drawn texture quad.
- Hardware rules: write every shader output, `projection[4]` first, test on real
  hardware (Azahar hides stalls and has wrong frame timing).

## Blockers to solve in psxrecomp for 3DS

| Area | Desktop today | 3DS plan |
|---|---|---|
| Overlays | spawn gcc/tcc, load a DLL | **static AOT overlays only** (`GAME_OVERLAY_STATIC_C`) + dirty-RAM interpreter fallback; disable `overlay_backend` / `overlay_posix` |
| Fibers | Win32 Fibers / POSIX `ucontext` (`psx_fiber.c`) | small ARM context-switch backend (or libctru threads + handoff) |
| Window/input/audio | SDL3 (~1,300 call sites, mostly `main.cpp`) | thin SDL shim like CTR-3D's `sdl3_compat_3ds.c`, or a `PSX_PLATFORM_3DS` host layer |
| Renderer | OpenGL (15k lines) / Vulkan / software | **phase 1: software rasterizer** (`gpu_sw_renderer.c`) blitted to the top screen; **phase 2: citro3d GP0 backend** (CPU-decoded VRAM texture cache, like CTR-3D's) |
| Launcher | recomp-ui (ImGui, setup wizard) | off; generation happens on the PC, the 3DS build links pre-generated C |
| Debug server, netplay, mods, OpenXR | on | off |
| Pointers | 64-bit hosts only so far | audit for 64-bit assumptions; 3DS is 32-bit ARMv6K |

Code generation always runs on the Mac (`psxrecomp_cli.py generate`). The 3DS
build only cross-compiles the generated C plus the runtime.

## Feasibility probe (2026-11-18)

Toolchain: devkitPro `3ds-dev` (devkitARM GCC 16.1, libctru, citro3d,
citro2d, picasso, `3DS.cmake`). A CMake hello-world builds to a `.3dsx`.

Every C/C++ file of the macOS build, minus SDL, recomp-ui and the recompiler,
was syntax-checked with `arm-none-eabi-gcc -march=armv6k -mfloat-abi=hard`:
**278 of 290 compile unchanged**, including all 94 generated THPS2 shards and
the recompiled BIOS. The 12 failures:

| File(s) | Cause | Fix |
|---|---|---|
| `mdec.c`, `timers.c`, `source_gpu_runtime.c` | newlib's `uint32_t` is `unsigned long`; GCC 16 rejects `unsigned*` → `uint32_t*` | fix types, or `-Wno-error=incompatible-pointer-types` |
| `mod_media.cpp`, `mod_runtime.cpp`, `render_pass_projection.cpp` | same: `std::min(unsigned, uint32_t)` | explicit template arg |
| `psx_fiber.c` | no `ucontext.h` | ARM context-switch backend |
| `overlay_posix.c` | no `dlfcn.h` | exclude (no runtime overlay compile on 3DS) |
| `main.cpp` | `ifaddrs.h` (netplay/LAN) | guard under a 3DS define |
| `debug_server.c`, `host_span_ring.c` | sockets, `rusage` fields | exclude (`PSX_NO_DEBUG_TOOLS`), or port debug server to 3DS sockets later |
| `boot_state_capture_jobs.c` | `zlib.h` | `dkp-pacman -S 3ds-zlib` |

SDL3 3.4.10 (vendored) has an official 3DS backend: ndsp audio, HID joystick,
threads and a **framebuffer** video driver (no GL). That gives a quick first
path: SDL3 host layer + psxrecomp software renderer → SDL framebuffer.

Overlays: on macOS THPS2 only ever executed 7 small RAM code pages (4–12 KB,
mostly BIOS kernel stubs around `0x80000000`). The interpreter covers them;
static AOT overlays are an optimization, not a blocker.

## 60 fps

THPS2 simulates at ~30 fps on PS1. Two separate goals:

1. **Full speed at stock cadence** (stable 30, no slowdown) on New 3DS at
   804 MHz with L2 cache. This is the real first milestone.
2. **60 fps presentation.** psxrecomp already supports presenting above the
   guest's cadence (`docs/FRAME_RATE.md`, `docs/FRAME_GENERATION.md`), but only
   on its OpenGL renderer. On 3DS this means the citro3d renderer has to record
   the frame's draw list and redraw it at an in-between camera. A true 60 Hz
   simulation would instead need game-specific patches (like CTR-3D's unlocked
   races), found by reverse-engineering THPS2's frame timing.

## Milestones

0. **macOS baseline**: generate + build + boot to gameplay on this Mac. This is
   the oracle every 3DS result is compared against.
1. **3DS skeleton**: runtime cross-compiles with devkitARM; boots the
   recompiled BIOS to the Sony logo on the software renderer.
2. **In-game**: static overlays, CD reads from `sdmc:`, pad input, menu loads.
3. **Audio**: SPU output through ndsp on core 3.
4. **Speed**: profile on hardware; citro3d renderer on core 2; full speed.
5. **Presentation**: widescreen option, stereoscopic 3D, bottom-screen UI.
6. **60 fps**: interpolated presentation, then evaluate native 60 Hz patches.
7. **Packaging**: CIA (full New 3DS app memory), release workflow.
