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

## Speed phase (started 2026-11-18)

Baseline in Azahar (`PSX_FPS_TELEMETRY=1` via `psx-env.txt`). **Azahar's
New 3DS mode does not raise the CPU clock:** it ignores `ConfigureNew3DSCPU`
(804 MHz + L2) and times the CPU at 268 MHz unless `cpu_clock_percentage` is
raised. Measured with the same build:

| Scene | 100% (268 MHz, Old 3DS budget) | 300% (804 MHz, New 3DS) |
|---|---|---|
| BIOS start | 5.4 fps | 16.2 fps |
| Sony logo | 2.0 fps | 6.0 fps |

All measurements from here on use 300%. `tools/azahar_run.py` reports three
windows: BIOS start (frames 20-70), Sony logo (125-200) and game start
(790-1000; the BIOS hands over to THPS2's code at frame 780). Changes to
game-code paths only show in the last one. The gap to 60 fps on the logo is
about 10x, not 30x. Azahar does not model the L2 cache; hardware numbers may
differ either way.

**Mac proxy.** `tools/mac_proxy_bench.py` runs a Mac build with the 3DS's
diagnostic settings (`build-mac3ds`), software renderer at native resolution,
turbo on, and reports guest fps over fixed frame ranges. Sony logo (frames
120-200): **162-167 fps on an M3 Pro vs 6.0 on the 3DS at 804 MHz, about
27x** (80x against the 268 MHz numbers). So for 60 fps on the 3DS the
runtime's cost per frame must drop roughly 10x. This is
a lean runtime profile for constrained hosts, not tuning. Each change is
measured on the proxy (seconds) and confirmed in Azahar.

Done so far:
- Direct presenter (`host_present.h` hook in psxrecomp, `n3ds_present_top`
  in `platform/n3ds`): the frame goes to the top-screen framebuffer in one
  rotated pass instead of three SDL CPU passes. Removed SDL's blitters from
  the profile; logo 1.9 -> 2.1 fps. Presentation was never the main cost.

Compiler settings (Azahar, 804 MHz; ARM11 is ARMv6K, so Thumb would be Thumb-1
and is not a candidate):

| Build | BIOS start | Sony logo | `.text` |
|---|---|---|---|
| `-O2` | 16.4 fps | 6.0 fps | 20.2 MB |
| **`-O3` (now the 3DS default, `THPS2_N3DS_OPT`)** | **17.3** | **6.6** | 20.4 MB |
| `-O3` runtime + `-Os` generated (`THPS2_N3DS_GENERATED_CFLAGS=-Os`) | 15.7 | 6.4 | 11.6 MB |
| `-Os` | 13.4 | 5.0 | 9.8 MB |

Azahar has no cache model, so it cannot reward smaller code. Compare `-O3`
against the mixed build on hardware before settling.

64-bit arithmetic: GDB breakpoints on the 64-bit division helpers found one hot
caller, the always-on guest-cycle profiler (`psx_cycle_sample_check`); it no
longer divides in the common case. Turning that profiler off entirely
(`PSX_GCP_PERIOD=0`) changed nothing measurable.

Where the time goes (Mac `sample`, emulation thread, logo/boot frames; guest
game code itself is a small minority):

| Cost | Notes |
|---|---|
| ~~Static-code validity check on every dispatch~~ **done** | THPS2's code and variables share 4 KB pages, so data writes flagged code pages and every dispatch re-compared its function's bytes. Now a word-precise "equal to the reference" bitmap (`memory.c`, `text_equal_*`) settles it on pages that only CPU stores touched. **Game start (frames 790-1000): 6.3 -> 9.6 fps (+52%)** in Azahar; BIOS phases unchanged (the check guards game code). `PSX_TEXT_EQUAL_BITS=0` turns it off for A/B. On the Mac it barely registered (`memcmp` is cheap on an M3) |
| Interrupt checks (`psx_check_interrupts*`, ~13%) | per dispatch / per block |
| Dirty-RAM bookkeeping on dispatch and writes | `dirty_ram_dispatch`, `dirty_ram_is_dirty` |
| Always-on per-write and per-call diagnostics | `debug_server_trace_write_check`, `debug_server_log_call_entry`, `xprobe_event`, `fntrace_record`, `mod_runtime_on_dispatch`, `text_xlate_on_dispatch`, `overlay_watch_note_write`, `history_record_live_write`; candidates for a compile-time "constrained host" profile |
| SPU catch-up, MDEC, display conversion (`gpu_display_pixel_*` per pixel) | |

Also open: `-O3`/LTO for the generated code on ARM11, and whether Azahar's CPU
timing matches the console (CTR-3D: "a change that only ran in the emulator
is unproven").

## Ceiling test (2026-11-18)

Question: can a lean runtime profile alone close the gap? Method: a Mac proxy
built like the 3DS (`build-maclean`: debug tools, launcher, netplay, rewind
off), run headless and unpaced (`PSX_UNPACED=1`; the Mac's SDL present is
vsync-bound and is not the 3DS's presenter), timed with the runtime's own
`PSX_BENCH_WINDOW`; then every changed step confirmed in Azahar.

| Step | Mac logo | Mac game start | 3DS BIOS start | 3DS logo | 3DS game start |
|---|---|---|---|---|---|
| Before | 202 | 444 | 17.3 | 6.6 | 9.6 |
| Verdict cache (code checks O(1) per call site; on everywhere) | 220 | 535 | 17.3 | 6.6 | 11.6 |
| `PSX_LEAN_HOST` (diagnostic store/dispatch hooks out; 3DS default) | 222 | 535 | **38.3** | 7.0 | 11.9 |
| `PSX_NATIVE_STATIC_CALLS` (not enabled: small, unverified) | 211 | 572 | 38.3 | 7.0 | 12.4 |

Classifying every sample of the lean proxy (emulation thread):

| | Logo | Game start |
|---|---|---|
| Guest code (game/BIOS compiled) | 11% | 16% |
| GPU raster, SPU, MDEC/CD/DMA | 12% | 13% |
| Interrupts + timing | 19% | 20% |
| Dispatch + code validity | 40% | 29% |
| Store bookkeeping | 7% | 10% |
| Other / unclassified | 12% | 11% |

**Answer:** with all runtime overhead gone the ceiling is ~3.0x (logo) and
~2.6x (game start); halving it gives ~1.5x. A lean profile alone does not
reach 60 fps, and reaches ~30 at game start only at its theoretical limit.
The remaining overhead is structural: interrupt checks at every block edge
and the per-call dispatch path (BIOS phases run kernel code from RAM-copied,
runtime-dirty pages, so they keep the full checks). The 3DS also paid far more
for the diagnostic store hooks than the proxy showed (BIOS start 2.2x on the
3DS vs +10% on the Mac).

## Gameplay baseline (2026-11-18)

`tools/n3ds/input-script-hangar.txt`, copied to `3ds/thps2/input-script.txt`,
presses through the menus into The Hangar by guest frame (the 3DS platform
layer ORs it into libctru's button reads through link-time wrappers).
`THPS2_SNAPSHOTS=<frames>` in `psx-env.txt` saves the presented frame as
`snap_<frame>.bmp` for checking a run without watching it.

| Window (Azahar, 804 MHz, lean profile) | fps | of PS1 speed |
|---|---|---|
| BIOS start (20-70) | 38.3 | 0.64x |
| Sony logo (125-200) | 7.0 | 0.12x |
| Game start (790-1000) | 11.9 | 0.20x |
| **Gameplay, The Hangar (7000-7400)** | **9.2** | **0.15x** |

Frames are vblanks (60 per PS1 second), so full speed in gameplay needs
about 6.5x. The 3DS run's snapshots at guest frames 7100 and 7350 are
identical to the Mac's at the same frames (pose, camera, timer): the
emulation is deterministic across hosts.

A full scripted run to frame 7410 takes about 15 minutes in Azahar.

**Gameplay profile** (100 GDB samples in The Hangar, `tools/azahar_profile.py
--classify`; raw stacks in `out/captures/n3ds-hangar/gameplay-stacks.txt`):

| Category | Share |
|---|---|
| Interrupts + timing (`psx_check_interrupts` 23%, `psx_cpu_charge` 5%) | 37% |
| GPU software rasteriser (shaded textured triangles, texel fetch) | 20% |
| Dispatch + code validity | 12% |
| GTE (geometry coprocessor) | ~10% |
| Waiting / system calls | 5% |
| Game code | 5% |
| Store bookkeeping, SPU, 64-bit helpers, CD/DMA | ~10% |

On the Mac proxy interrupts were 15-20%; on the in-order ARM11 they lead.
`psx_check_interrupts` is 2,232 instructions with a 524-byte frame, called
at every basic-block edge of game code; samples spread over its prologue and
epilogue, `psx_netplay_active()` and `psx_interrupt_delivery_needed()` calls
(netplay off), the savestate-poll maintenance block and `event_ring_record`.
Nothing in it is slow alone; the full function runs thousands of times per
frame to find nothing to deliver.

**Next:** an interrupt-check fast path at the block-edge call site that
settles "nothing pending, nothing due" inline while keeping what the full
function guarantees (cycle-batch flush, periodic maintenance, netplay rules).
After that, a PICA200 renderer is the other large lever (rasteriser + GTE
~30%).

## Observation time (2026-11-18)

psxrecomp already had the interrupt fast path: `PSX_OBSERVATION_TIME` keeps
ordinary block edges in an inline policy and calls the full check only on
slow edges. Generated code already carries its boundary marker. Now on for
3DS builds. Its edge was `always_inline` into every block edge, which grew
the code 22.6 -> 36.3 MB and ran the 3DS out of heap at startup; on
`PSX_LEAN_HOST` the edge is one out-of-line call (`psx_block_edge_call`),
code 23.0 MB.

| Window (Azahar) | Before | Observation time |
|---|---|---|
| BIOS start | 38.3 | 38.8 |
| Sony logo | 7.0 | 7.5 |
| Game start | 11.9 | 14.8 |
| Gameplay | 9.2 | 9.3 |

Gameplay frames 7100/7350 pixel-identical with it off and on, on macOS and
the 3DS. Gameplay barely moved: during play most edges are slow edges (a
device deadline is due), which the next profile has to explain.

Roadmap: an out-of-memory failure (`std::bad_alloc`) on the 3DS currently
ends the process silently; report it on screen.

## Roadmap: disc image format and disc-read cost

Requested 2026-11-18: check whether the compressed CHD (vs a raw BIN/ISO)
slows the game.

Known so far: the 3DS build does not read the CHD. `game.toml` names
`disc/Tony Hawk's Pro Skater 2 (USA).cue`, which `psxrecomp_cli.py generate`
extracted from the CHD as a raw MODE2/2352 `.bin`, and
`tools/n3ds_stage_sd.py` copies only that `.bin`/`.cue` to the SD card. The
macOS builds read the same `.bin`. In the gameplay profile, CD/DMA data work
is about 1%.

Still to measure:
- Disc-read cost on real SD-card speeds: time spent in CD sector reads during
  level loads and XA music/FMV streaming (Azahar's SD emulation is a host
  file and much faster than a console's card).
- Whether streaming reads stall the frame (CTR-3D needed a read cache and
  prefetch for XA streams on hardware).
- For completeness, a CHD-backed run on the Mac (the runtime can mount a CHD
  through libchdr) to quantify decompression cost, should a CHD ever be used
  directly.

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
