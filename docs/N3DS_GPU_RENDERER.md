# PICA200 renderer for the THPS2 3DS port: design

Status: design, nothing implemented. Written 2026-11-19 after the first
hardware gameplay profile.

## Why

New 3DS, The Hangar, Luma3DS HBL, build 05a1b7d: gameplay runs at **5.2 fps**
(0.09x PS1). On the game thread the software rasteriser and display
conversion are **35%** of samples (shaded textured triangles and texel fetch
dominate) and the GTE about 10% (see `N3DS_PORT_PLAN.md`, "Gameplay on
hardware"). The PICA200 can rasterise and texture those triangles; no other
single change reaches as much of the frame.

What the GPU renderer does not fix: dispatch, interrupts and timing, store
bookkeeping (together ~30-35%). Expect a large step, not 60 fps by itself.

## Constraints

From the hardware and from CTR-3D (`zando95/CTR-3D`, `tinaut1986/CTR-3D`,
`platform/n3ds/native_renderer_3ds.c` and `docs/3DS_PORT.md`), which solved
the same PS1-to-PICA200 problem for a decompiled game:

- The PICA200 has **no fragment shaders and no palette textures**. PS1 4-bit
  and 8-bit CLUT textures must be decoded on the CPU into RGBA textures, held
  in a cache keyed by (texture page, CLUT, format) and invalidated by VRAM
  writes to the rows they were decoded from.
- Fixed-function texture combiners can do what PS1 shading needs: texel x
  vertex colour x 2 (combiner scale 2x), raw texture, flat/gouraud colour.
- Blending: the four PS1 semi-transparency modes (B/2+F/2, B+F, B-F, B+F/4)
  map to PICA blend equations with a constant blend colour for the 0.5 and
  0.25 factors. PS1 blends per texel only where the texel's STP bit is set;
  CTR-3D draws semi-transparent textured primitives twice (opaque texels,
  then blended texels via alpha test on a decoded STP alpha).
- Mask bit (GP0 E6h: set/check bit 15) maps to the stencil buffer.
- Rules emulators do not enforce (CTR-3D): write every component of every
  vertex-shader output; `projection[4]` first in every program; `mul` cannot
  take a uniform as its second operand.
- 3DS program image limit: 64 MB (now 55.7 MB). Renderer code is small, but
  GPU buffers must come from linear memory (currently 8 MB, adjustable) and
  textures/targets from VRAM (6 MB).

## The architectural choice

psxrecomp emulates the PS1 GPU and hands primitives to a backend through
`GpuRenderBackend` (`runtime/include/gpu_render.h`, ~40 operations: draw
triangles/rects/lines, fill/copy/upload/read VRAM, present). The software,
OpenGL and Vulkan renderers implement it. The OpenGL backend is
"GPU-authoritative VRAM": the whole 1024x512 VRAM lives on the GPU and the CPU
copy is synchronised on demand. Its 15,000 lines lean on fragment shaders and
cannot be ported.

**Chosen design: CPU VRAM stays authoritative; the GPU draws the frames that
are displayed** (CTR-3D's model, adapted to a command-stream emulator):

1. The software backend keeps owning PS1 VRAM: uploads, fills, copies,
   readback, texture source data, MDEC/FMV frames.
2. Draws whose draw area is a **display buffer** (the VRAM rectangle the game
   later scans out) are recorded per frame and drawn by the PICA200 into a
   render target for that buffer, in submission order (painter's order, no
   depth test), batched by texture and blend state.
3. Draws into anything else (off-screen render-to-texture) stay on the CPU
   rasteriser, into VRAM.
4. Presentation: when the game displays a buffer the GPU owns, the GPU target
   goes to the top screen directly (no CPU conversion). When it displays a
   CPU-owned region (FMV, loading screens drawn by fills/uploads), the
   existing CPU presenter shows VRAM as today.
5. **Coherence**: if anything reads a GPU-owned region from the CPU side
   (texture sampled from it, `copy_rect` out of it, `vram_transfer_out`, a
   draw that mixes CPU and GPU into one buffer), that region is read back from
   the GPU (GX display transfer to linear memory, convert to 1555) and returns
   to CPU ownership. Correct always; fast only if rare.

Whether this is fast depends on how often step 5 happens in THPS2. That is
measured first (Phase 0) rather than assumed.

## Where the code lives

- psxrecomp (fork, `n3ds`): one small hook, a platform backend registration
  (`gr_register_platform_backend`), in the style of `host_present.h`. No
  citro3d in the framework.
- This repo, `platform/n3ds/`: the PICA backend (citro3d, shaders built with
  picasso), the texture decode cache, the coherence tracker. libctru-facing
  code in its own files, as now.

## Phases (each ends measured on hardware, frames compared)

**Phase 0 - measure THPS2's VRAM behaviour (Mac, a few hours).** A recording
wrapper around the software backend counts, per frame: draw-area targets and
how many primitives each gets, which regions get displayed, texture pages
sampled from regions drawn earlier, `copy_rect`/`vram_transfer_out` touching
drawn regions, FMV uploads. Output: how much of the frame goes to display
buffers, and how many readbacks the design would need per frame in menus and
gameplay. If readbacks are frequent, revisit before Phase 1.

**Phase 1 - citro3d skeleton (3DS).** citro3d initialised alongside SDL's gfx
setup; the existing presenter switches to a citro3d top-screen target fed
from the CPU frame (same output as today, now through the GPU path).
Registration hook in psxrecomp. Gate: identical frames, no fps loss.

**Phase 2 - untextured primitives on the GPU.** Flat/gouraud triangles,
rects, lines and fills into display buffers; ordering, draw area (scissor),
draw offset, the four blend modes, mask bit. Everything else CPU. Gate: menus
and gameplay frames match the software renderer except for documented
rasterisation-rule differences (edge pixels).

**Phase 3 - textured primitives.** CPU decode cache for 4/8/15-bit textures
with CLUT and texture window, keyed and invalidated as above; modulation x2,
raw texture, STP two-pass. This is where the 35% moves. Gate: frame compare
plus hardware fps.

**Phase 4 - coherence and presentation.** Readback path, CPU-owned display
regions, FMV, the pause/feedback effects THPS2 uses (Phase 0 tells which).

**Phase 5 - move work off the game thread.** Vertex building and GPU
submission on a render worker on core 2 (CTR-3D's layout), and only then the
GTE question.

## Verification

- Frame-exact comparison is not the goal (GPU edge rules differ from the PS1
  rasteriser). Comparison is against the software renderer at the same guest
  frames (the deterministic scripted run), with a pixel-difference budget per
  frame and side-by-side sheets; any structural difference (missing or extra
  primitives, wrong blending, wrong textures) is a bug.
- Emulation must stay identical: the GPU renderer may not change guest state.
  Guest-visible results (VRAM contents the game reads back) go through the
  coherence path, so `frame_fingerprint`-style checks on guest state still
  match.
- Hardware fps from `log.txt` at the same windows as the baseline (BIOS
  start, logo, game start, intro, menus, gameplay 7000-7400 and 9000-13500).
