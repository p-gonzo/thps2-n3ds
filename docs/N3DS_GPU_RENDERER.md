# PICA200 renderer for the THPS2 3DS port: design

Status: Phases 1 and 2 done (2026-11-19); Phase 3 (textures) next. Phase 0 showed the design holds for THPS2 ("Phase 0 results").

**Aspect ratio rule:** the PS1 picture is always shown at 4:3, 320x240
centred on the 400x240 top screen with black bars. Nothing is ever stretched
to fill the screen. True widescreen (rendering more of the scene) is a later,
separate roadmap item.

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

**Phase 0 - measure THPS2's VRAM behaviour (Mac). Done; results below.** A recording
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

*Done (2026-11-19):* `platform/n3ds/n3ds_gpu.c` + `shaders/present.v.pica`.
citro3d starts on the first present (after SDL's gfxInit); the frame goes to
a VRAM texture (CPU ARGB->RGBA8 into a linear staging image, then a GX
display transfer) and a 320x240 quad on a 400x240 target. Nearest sampling
with coordinates shifted half an output pixel picks exactly the CPU
presenter's source pixels (an epsilon of 1/1024 pixel was below the PICA's
texture-coordinate precision and picked the column to the left; 1/32 is
safe for every PS1 width). Results in Azahar, GPU vs CPU presenter
(`THPS2_PRESENT=cpu`), comparing what is actually on the top screen
(`screen_<frame>.bmp`): 0 differing pixels at the 640x478 BIOS screen, the
320x240 intro video, the menu and four gameplay frames; gameplay 9.35 fps
both ways. No psxrecomp hook was needed: the existing host-present
registration carries the frame.

Hardware (New 3DS, 2026-11-19, Hangar save state): gameplay median 5.20 fps,
the same as the 5.2 fps CPU-presenter baseline; `screen_600.bmp` shows the
picture in columns 40-359 of 400 (4:3, not stretched). A Rosalina profile put
`n3ds_gpu_present` at ~5% of the game thread (its CPU colour conversion; the
runtime's own ARGB scan-out is another ~5%; both go away in Phase 3).

**Phase 2 - untextured primitives on the GPU.** Flat/gouraud triangles,
rects, lines and fills into display buffers; ordering, draw area (scissor),
draw offset, the four blend modes, mask bit. Everything else CPU. Gate: menus
and gameplay frames match the software renderer except for documented
rasterisation-rule differences (edge pixels).

*Done (2026-11-19) as a shadow renderer.* Splitting one frame between a GPU
painter (untextured) and a CPU painter (textured) would need a sync at every
switch, so until Phase 3 the software renderer still draws and presents
everything, and the GPU draws in parallel into its own copy of VRAM:

- psxrecomp: `gr_set_host_tap()` (gpu_render.c) hands every renderer call to
  the platform after the backend ran it. Diagnostics for comparisons:
  `PSX_GR_SKIP_TEXTURED=1` drops textured primitives in every renderer;
  `PSX_GR_TEXTURED_AS_SHADES=1` redraws them as gouraud shapes coloured per
  texture page and lit by the vertex colours, so the full geometry, overlaps
  and blending can be compared without textures.
- 3DS: `platform/n3ds/n3ds_gpu_shadow.c` + `shaders/draw.v.pica`, enabled by
  `THPS2_GPU_SHADOW=1`. Primitives are batched per (blend mode, draw area) in
  a double-buffered linear vertex buffer and drawn at present time, in
  submission order, into a 1024x512 **RGBA5551** texture/target (5 bits per
  channel like the PS1: stacked additive layers then round like the
  software path; alpha holds the mask bit). Draw area = scissor; vertices
  get +0.5 so GPU pixel centres sample where the PS1 rules do; rectangles and
  fills sit on pixel edges. Blend: B/2+F/2 constant alpha 0.5; B+F one/one;
  B-F reverse subtract; B+F/4 constant alpha 0.25. Snapshots add
  `gpu_<frame>.bmp` (the GPU copy of the displayed buffer).
- What THPS2 uses (scripted boot into The Hangar): 210k triangles (12.5k
  gouraud), 142k rectangles, 1.8k fills; opaque, B+F (11.8k) and B-F (84k)
  blending; mask bit never; lines never; 2 copies (the menu frame grabs).
- Results in Azahar, GPU copy vs software frame, 5-bit channels: no missing
  or extra primitives and no coverage differences. With
  `PSX_GR_TEXTURED_AS_SHADES=1`: menus 98-100% exact (character select:
  2,272 pixels 1 step, 272 at 2, 8 beyond); gameplay ~55% exact, ~45% one
  step, <0.7% two steps, ~0.01% more. The steps are gouraud rounding: the
  software path interpolates in 5-bit steps (visible banding), the GPU in
  8 bits stored as 5. Neither is the PS1's 8-bit interpolation with
  dithering; matching that is a later fidelity item, not a structural bug.
- Not mirrored yet (Phase 4): VRAM uploads and copies into display buffers.

**Phase 3 - textured primitives.** CPU decode cache for 4/8/15-bit textures
with CLUT and texture window, keyed and invalidated as above; modulation x2,
raw texture, STP two-pass. This is where the 35% moves. Gate: frame compare
plus hardware fps.

**Phase 4 - coherence and presentation.** Readback path, CPU-owned display
regions, FMV, the pause/feedback effects THPS2 uses (Phase 0 tells which).

**Phase 5 - move work off the game thread.** Vertex building and GPU
submission on a render worker on core 2 (CTR-3D's layout), and only then the
GTE question.

## Phase 0 results (2026-11-19)

Tool: psxrecomp's `vram_census` debug-server command (`gpu_vram_census.c`,
inert until armed) driven by `tools/vram_census.py` along the scripted path
into The Hangar (Mac debug build, software renderer). It simulates the
ownership model above on 16x16 cells; raw output in `out/census.json`.

| Window (guest frames) | Presents | Shown from GPU | Primitives | Readbacks |
|---|---|---|---|---|
| Sony logo (130-200) | 70 | 70 | 186 | 0 |
| Game start (790-1000) | 66 | 11 (rest CPU: boot screens) | 4 | 1 (one full-VRAM clear) |
| Intro FMV (1200-1700) | 500 | 0 (24-bit MDEC, CPU) | 0 | 0 |
| Menus (1800-3200) | 1400 | 1379 | 45,388 | 0 |
| Menus + loading (3300-6500) | 3200 | 1919 (+1182 partly CPU) | 446,958 | 4 (124 cells) |
| Gameplay (7000-7400) | 400 | 400 | 264,860 | 0 |
| Gameplay (7400-9000) | 1600 | 1600 | 1,128,439 | 0 |

What it means:

- **Every primitive in menus and gameplay is drawn into the two display
  buffers**, (0,0) and (0,256), 512x240, alternating. THPS2 does no
  render-to-texture: no draw samples a region drawn earlier, no copy or
  GPUREAD pulls drawn pixels back during gameplay. The only readbacks in the
  whole run are two 176x48 framebuffer grabs in the menus (copied to
  (703,256)) and a full-VRAM clear at boot.
- So the GPU can own both display buffers for the whole of gameplay with zero
  CPU synchronisation, and the CPU rasteriser is needed only for those rare
  cases (handled by Phase 4's readback path, not a second renderer).
- Workload: the game renders at 30 fps (one fill/clear every two vblanks), so
  a rendered frame has **~1,400 primitives, 64% textured**. Small for the
  PICA200; the cost will be CPU-side vertex building and texture decode.
- The one per-frame VRAM copy is a 2x1 copy of (0,0) onto itself (a no-op
  idiom); it needs no GPU work.
- Presentation: 512 wide onto the 400-pixel top screen. The PICA200 can draw
  straight into a 400x240 target with a horizontal 400/512 projection, so
  there is no separate scaling pass. The intro FMV is 24-bit MDEC uploaded
  by the CPU and stays on the CPU presenter.
- "Partly CPU" presents in the loading window are display cells no draw
  touched yet (stale VRAM): the GPU target needs a one-time upload from CPU
  VRAM when a buffer first becomes a target (cheap, rare).

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
