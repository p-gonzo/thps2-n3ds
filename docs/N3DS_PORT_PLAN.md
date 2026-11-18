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

## Milestone 1 progress (2026-11-18)

The real psxrecomp build now configures, compiles and **links** for the 3DS
(psxrecomp `n3ds` `46ef4d9`):

```bash
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
cmake -S . -B build-3ds -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake \
  -DCMAKE_BUILD_TYPE=Release -DPSX_RECOMP_UI=OFF -DPSX_ENABLE_VULKAN=OFF \
  -DPSX_DEBUG_TOOLS=OFF -DPSX_NETPLAY=OFF -DPSX_REWIND=OFF
ninja -C build-3ds psx-runtime
```

What changed in the framework: GL 1.1 is loaded through SDL so nothing links
against GL (the 3DS falls back to the software renderer); an ARM fiber backend;
honest "unavailable" paths where Horizon cannot spawn, exec or dlopen; a few
portability fixes. macOS OpenGL and software renderers still reach gameplay at
full speed.

**Blocker: memory.** `.text` 21 MB and `.rodata` 27 MB fit, but `.bss` is
**741 MB** against roughly 124 MB of New 3DS application memory. About 600 MB
of it is diagnostic recording rings compiled into every build (largest:
`fntrace.c` 144 MB, `sio.c` 102 MB, `debug_server.c` 101 MB,
`audio_trace.c` 80 MB, `parity_trace.c` 50 MB, `overlay_loader.c` 48 MB).
Next step: one build-level capacity setting for those rings, so a
memory-limited host sizes them down or leaves them out.

## Milestone 1 reached (2026-11-18)

In Azahar 2126.2 (New 3DS mode) the 3DS build boots the recompiled SCPH-1001
BIOS to the Sony logo and hands over to THPS2's own code (Activision logo), at
about 2 fps. psxrecomp `n3ds` `3cfb220`.

What it took, in order of discovery:

| Problem | Fix |
|---|---|
| 741 MB `.bss`, ~700 MB heap rings | `PSX_DIAG_CAP` on 51 diagnostic sizes (shift 6 on 3DS); DLL-only overlay tables minimal on 3DS. Static total now ~97 MB |
| libctru's default 32 MB linear heap left no normal heap, so it died before `main` | `__ctru_linear_heap_size` = 8 MB (`platform/n3ds/n3ds_platform.c`) |
| Every C++ throw ended in `std::terminate` | devkitARM `3dsx.ld` sets `__exidx_start` before the section's alignment padding; `__gnu_Unwind_Find_exidx` returns the aligned table |
| `sdmc:/...` is "relative" to libstdc++ (canonical, absolute, doubled paths) | `host_path.h` treats `name:/` as absolute; `psx_path_canonical.h` |
| Full-disc SHA-256 on every launch (~285 s) | per-image digest cache (`PSX_DISC_DIGEST_CACHE`) |
| `temp_directory_path` throws (no temp dir) | cache root in the data folder |
| No BIOS picker | staged `bios.cfg`, the launcher's own remembered-pick file |
| SDL's 3DS driver refuses a GL window | window falls back to the software renderer |

Report upstream: the `3dsx.ld` exidx bug to devkitPro; the PSX_RECOMP_UI=OFF
compile fix, GL-through-SDL loader and host_path device roots to
RetroPortingToolKit.

**Where the time goes** (15 GDB stack samples during the BIOS intro): about a
third in one BIOS routine (`SCPH1001_func_1FC41DC8`) and the interrupt checks
around it, which looks like the BIOS busy-waiting for vblank (`idle_skip` is
disabled); then the software rasterizer (Gouraud triangles), the per-pixel
display conversion, and `fntrace_record`, which runs on every dispatched call.
Next: measure on hardware-like settings, idle skip, take the always-on tracing
out of the 3DS hot path, then the renderer.

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
