/* n3ds_gpu_shadow.c — GPU renderer Phase 2 (docs/N3DS_GPU_RENDERER.md).
 *
 * Shadow mode: the software renderer still draws and presents everything.
 * Through psxrecomp's host tap (gr_set_host_tap) this file also receives
 * every primitive and draws the untextured ones (flat/gouraud triangles,
 * rectangles, fills) with the PICA200 into a 1024x512 RGBA5551 copy of PS1
 * VRAM, in submission order, with the PS1's draw area as scissor, its four
 * semi-transparency modes as blend equations, and the mask bit in alpha.
 * Textured primitives are only counted until Phase 3.
 *
 * Enabled by THPS2_GPU_SHADOW=1 (psx-env.txt). Checked by running with
 * PSX_GR_SKIP_TEXTURED=1 too (both renderers then draw only untextured
 * primitives) and comparing gpu_<frame>.bmp with snap_<frame>.bmp. */
#include "n3ds_gpu_shadow.h"

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "draw_shbin.h"     /* shaders/draw.v.pica */
#include "gpu_render.h"     /* psxrecomp: GpuRenderBackend, gr_set_host_tap */

enum { VW = 1024, VH = 512, MAX_VERTS = 65536, MAX_BATCHES = 4096 };

typedef struct { float x, y; uint8_t r, g, b, a; } Vtx;   /* 12 bytes */
typedef struct {
    uint32_t first, count;
    uint8_t  blend;                 /* 0 opaque, 1 + PS1 semi mode */
    uint16_t sx1, sy1, sx2, sy2;    /* scissor, inclusive VRAM pixels */
} Batch;

static int       s_on = -1;         /* THPS2_GPU_SHADOW, read once */
static int       s_ready;           /* GPU objects exist */
static int       s_cleared;
static float     s_bias = 0.5f;     /* PS1 samples pixel (x, y) at integer coords */
static C3D_Tex   s_vram;            /* GPU copy of PS1 VRAM, also a render target */
static C3D_RenderTarget *s_target;
static DVLB_s   *s_dvlb;
static shaderProgram_s s_prog;
static int       s_proj_loc;
static C3D_Mtx   s_proj;
static Vtx      *s_vbo[2];          /* double-buffered: GPU reads one while we fill the other */
static int       s_cur;
static uint32_t  s_nverts;
static Batch     s_batches[MAX_BATCHES];
static uint32_t  s_nbatches;
static uint32_t *s_capture;         /* linear VW x VH, for snapshots */

/* PS1 drawing state as last set through the facade. */
static int s_semi, s_semi_mode, s_mask_set, s_mask_check;
static int s_ax1, s_ay1, s_ax2 = VW - 1, s_ay2 = VH - 1;

/* Counters, reported once a second by the HUD status line. */
static struct {
    uint32_t tris, rects, fills, textured, lines, copies, uploads, mask_check, dropped;
    uint32_t blend[5];   /* primitives per mode: opaque, B/2+F/2, B+F, B-F, B+F/4 */
    uint32_t gouraud, mask_set;
} s_n;

int n3ds_shadow_enabled(void)
{
    if (s_on < 0) {
        const char *e = getenv("THPS2_GPU_SHADOW");
        s_on = e && e[0] == '1';
        const char *b = getenv("THPS2_GPU_BIAS");
        if (b) s_bias = strtof(b, NULL);
    }
    return s_on;
}

/* ---- Recording (called on the game thread through the tap) --------------- */

static Batch *batch_for(int blend, int x1, int y1, int x2, int y2)
{
    Batch *b = s_nbatches ? &s_batches[s_nbatches - 1] : NULL;
    if (b && b->blend == blend && b->sx1 == x1 && b->sy1 == y1 && b->sx2 == x2 && b->sy2 == y2 &&
        b->first + b->count == s_nverts)
        return b;
    if (s_nbatches == MAX_BATCHES) return NULL;
    b = &s_batches[s_nbatches++];
    b->first = s_nverts; b->count = 0; b->blend = (uint8_t)blend;
    b->sx1 = (uint16_t)x1; b->sy1 = (uint16_t)y1; b->sx2 = (uint16_t)x2; b->sy2 = (uint16_t)y2;
    return b;
}

static inline uint8_t c5to8(unsigned c) { return (uint8_t)((c << 3) | (c >> 2)); }

static Vtx *reserve(int n, int blend, int x1, int y1, int x2, int y2)
{
    if (!s_vbo[s_cur] || s_nverts + (uint32_t)n > MAX_VERTS) { s_n.dropped++; return NULL; }
    Batch *b = batch_for(blend, x1, y1, x2, y2);
    if (!b) { s_n.dropped++; return NULL; }
    Vtx *v = &s_vbo[s_cur][s_nverts];
    s_nverts += (uint32_t)n;
    b->count += (uint32_t)n;
    return v;
}

static inline void put(Vtx *v, float x, float y, uint16_t c, uint8_t a)
{
    v->x = x; v->y = y;
    v->r = c5to8(c & 31); v->g = c5to8((c >> 5) & 31); v->b = c5to8((c >> 10) & 31);
    v->a = a;
}

static int cur_blend(void) { const int b = s_semi ? 1 + (s_semi_mode & 3) : 0; s_n.blend[b]++; if (s_mask_set) s_n.mask_set++; return b; }

static void tri(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1,
                int x2, int y2, uint16_t c2)
{
    if (s_mask_check) s_n.mask_check++;
    Vtx *v = reserve(3, cur_blend(), s_ax1, s_ay1, s_ax2, s_ay2);
    if (!v) return;
    const uint8_t a = s_mask_set ? 255 : 0;
    const float o = s_bias;
    put(&v[0], (float)x0 + o, (float)y0 + o, c0, a);
    put(&v[1], (float)x1 + o, (float)y1 + o, c1, a);
    put(&v[2], (float)x2 + o, (float)y2 + o, c2, a);
}

/* Axis-aligned rectangle covering pixels [x, x+w) x [y, y+h): its edges sit on
 * pixel boundaries, so it needs no sampling bias. */
static void rect(int x, int y, int w, int h, uint16_t c, uint8_t a, int blend,
                 int x1, int y1, int x2, int y2)
{
    Vtx *v = reserve(6, blend, x1, y1, x2, y2);
    if (!v) return;
    const float l = (float)x, t = (float)y, r = (float)(x + w), b = (float)(y + h);
    put(&v[0], l, t, c, a); put(&v[1], r, t, c, a); put(&v[2], l, b, c, a);
    put(&v[3], r, t, c, a); put(&v[4], r, b, c, a); put(&v[5], l, b, c, a);
}

static void t_set_semi(int e, int m) { s_semi = e; s_semi_mode = m; }
static void t_set_mask(int set, int check) { s_mask_set = set; s_mask_check = check; }
static void t_set_area(int x1, int y1, int x2, int y2)
{
    s_ax1 = x1 < 0 ? 0 : x1 > VW - 1 ? VW - 1 : x1;
    s_ay1 = y1 < 0 ? 0 : y1 > VH - 1 ? VH - 1 : y1;
    s_ax2 = x2 < 0 ? 0 : x2 > VW - 1 ? VW - 1 : x2;
    s_ay2 = y2 < 0 ? 0 : y2 > VH - 1 ? VH - 1 : y2;
}
static void t_flat_tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c)
{ s_n.tris++; tri(x0, y0, c, x1, y1, c, x2, y2, c); }
static void t_gouraud_tri(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1,
                          int x2, int y2, uint16_t c2)
{ s_n.tris++; s_n.gouraud++; tri(x0, y0, c0, x1, y1, c1, x2, y2, c2); }
static void t_flat_rect(int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) return;
    s_n.rects++;
    if (s_mask_check) s_n.mask_check++;
    rect(x, y, w, h, c, s_mask_set ? 255 : 0, cur_blend(), s_ax1, s_ay1, s_ax2, s_ay2);
}
/* GP0(02h): ignores draw area, mask and blending; the colour's bit 15 is
 * written as is. Wrapping fills (past the VRAM edge) are rare: counted as
 * dropped rather than split. */
static void t_fill(int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) return;
    s_n.fills++;
    if (x < 0 || y < 0 || x + w > VW || y + h > VH) { s_n.dropped++; return; }
    rect(x, y, w, h, c, (c & 0x8000) ? 255 : 0, 0, 0, 0, VW - 1, VH - 1);
}
static void t_copy(int sx, int sy, int dx, int dy, int w, int h)
{
    if (sx == dx && sy == dy) return;   /* THPS2's per-frame 2x1 self-copy */
    (void)w; (void)h;
    s_n.copies++;                      /* not mirrored yet (Phase 4) */
}
static void t_line(int x0, int y0, int x1, int y1, uint16_t c)
{ (void)x0; (void)y0; (void)x1; (void)y1; (void)c; s_n.lines++; }
static void t_shaded_line(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1)
{ (void)x0; (void)y0; (void)c0; (void)x1; (void)y1; (void)c1; s_n.lines++; }
static void t_tex_tri(int x0, int y0, int u0, int v0, int x1, int y1, int u1, int v1,
                      int x2, int y2, int u2, int v2, uint16_t cx, uint16_t cy, uint16_t tp)
{
    (void)x0; (void)y0; (void)u0; (void)v0; (void)x1; (void)y1; (void)u1; (void)v1;
    (void)x2; (void)y2; (void)u2; (void)v2; (void)cx; (void)cy; (void)tp;
    s_n.textured++;
}
static void t_shaded_tex_tri(int x0, int y0, int u0, int v0, uint32_t c0,
                             int x1, int y1, int u1, int v1, uint32_t c1,
                             int x2, int y2, int u2, int v2, uint32_t c2,
                             uint16_t cx, uint16_t cy, uint16_t tp, int raw)
{
    (void)x0; (void)y0; (void)u0; (void)v0; (void)c0; (void)x1; (void)y1; (void)u1; (void)v1;
    (void)c1; (void)x2; (void)y2; (void)u2; (void)v2; (void)c2; (void)cx; (void)cy; (void)tp;
    (void)raw;
    s_n.textured++;
}
static void t_tex_rect(int x, int y, int w, int h, int u, int v, uint16_t cx, uint16_t cy,
                       uint16_t tp)
{
    (void)x; (void)y; (void)w; (void)h; (void)u; (void)v; (void)cx; (void)cy; (void)tp;
    s_n.textured++;
}
static void t_tex_rect_scaled(int x, int y, int w, int h, int u0, int v0, int u1, int v1,
                              uint16_t cx, uint16_t cy, uint16_t tp)
{
    (void)x; (void)y; (void)w; (void)h; (void)u0; (void)v0; (void)u1; (void)v1;
    (void)cx; (void)cy; (void)tp;
    s_n.textured++;
}
static void t_upload(int x, int y, int w, int h, const uint16_t *d)
{ (void)x; (void)y; (void)w; (void)h; (void)d; s_n.uploads++; }

static const GpuRenderBackend k_tap = {
    .name = "n3ds-shadow",
    .set_semi_transparency = t_set_semi,
    .set_mask_bits = t_set_mask,
    .set_draw_area = t_set_area,
    .fill_rect = t_fill,
    .copy_rect = t_copy,
    .draw_flat_triangle = t_flat_tri,
    .draw_gouraud_triangle = t_gouraud_tri,
    .draw_flat_rect = t_flat_rect,
    .draw_line = t_line,
    .draw_shaded_line = t_shaded_line,
    .draw_textured_triangle = t_tex_tri,
    .draw_shaded_textured_triangle = t_shaded_tex_tri,
    .draw_textured_rect = t_tex_rect,
    .draw_textured_rect_scaled = t_tex_rect_scaled,
    .vram_transfer_in = t_upload,
};

void n3ds_shadow_install(void)
{
    if (!n3ds_shadow_enabled()) return;
    s_vbo[0] = linearAlloc(sizeof(Vtx) * MAX_VERTS);
    s_vbo[1] = linearAlloc(sizeof(Vtx) * MAX_VERTS);
    if (!s_vbo[0] || !s_vbo[1]) {
        printf("thps2-n3ds: GPU shadow off: no linear memory for vertices\n");
        s_on = 0;
        return;
    }
    gr_set_host_tap(&k_tap);
    printf("thps2-n3ds: GPU shadow renderer on (untextured primitives, bias %.2f)\n", (double)s_bias);
}

/* ---- Drawing (inside the present's citro3d frame) ------------------------ */

static int gpu_objects(void)
{
    if (s_ready) return s_ready > 0;
    s_ready = -1;
    if (!C3D_TexInitVRAM(&s_vram, VW, VH, GPU_RGBA5551)) {
        printf("thps2-n3ds: GPU shadow off: no VRAM for the 1024x512 target\n");
        return 0;
    }
    s_target = C3D_RenderTargetCreateFromTex(&s_vram, GPU_TEXFACE_2D, 0, -1);
    if (!s_target) {
        printf("thps2-n3ds: GPU shadow off: render target failed\n");
        return 0;
    }
    s_dvlb = DVLB_ParseFile((u32 *)draw_shbin, draw_shbin_size);
    shaderProgramInit(&s_prog);
    shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
    s_proj_loc = shaderInstanceGetUniformLocation(s_prog.vertexShader, "projection");
    /* VRAM pixels, y = 0 at framebuffer row 0. */
    Mtx_Ortho(&s_proj, 0.0f, (float)VW, 0.0f, (float)VH, 0.0f, 1.0f, true);
    s_ready = 1;
    return 1;
}

static void set_blend(int blend)
{
    switch (blend) {
    case 1:  /* B/2 + F/2 */
        C3D_BlendingColor(0x80000000u);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA,
                       GPU_ONE, GPU_ZERO);
        break;
    case 2:  /* B + F */
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ZERO);
        break;
    case 3:  /* B - F */
        C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ZERO);
        break;
    case 4:  /* B + F/4 */
        C3D_BlendingColor(0x40000000u);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_ALPHA, GPU_ONE, GPU_ONE, GPU_ZERO);
        break;
    default:
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        break;
    }
}

void n3ds_shadow_render(void)
{
    if (!n3ds_shadow_enabled() || !gpu_objects()) { s_nverts = s_nbatches = 0; return; }
    if (!s_cleared) { C3D_RenderTargetClear(s_target, C3D_CLEAR_ALL, 0, 0); s_cleared = 1; }
    if (!s_nbatches) return;
    GSPGPU_FlushDataCache(s_vbo[s_cur], s_nverts * sizeof(Vtx));
    C3D_FrameDrawOn(s_target);
    C3D_BindProgram(&s_prog);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_proj_loc, &s_proj);
    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 1, GPU_UNSIGNED_BYTE, 4);
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, s_vbo[s_cur], sizeof(Vtx), 2, 0x10);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    for (uint32_t i = 0; i < s_nbatches; i++) {
        const Batch *b = &s_batches[i];
        if (!b->count) continue;
        set_blend(b->blend);
        C3D_SetScissor(GPU_SCISSOR_NORMAL, b->sx1, b->sy1, b->sx2 + 1u, b->sy2 + 1u);
        C3D_DrawArrays(GPU_TRIANGLES, (int)b->first, (int)b->count);
    }
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    s_cur ^= 1;
    s_nverts = s_nbatches = 0;
}

int n3ds_shadow_capture(int x, int y, int w, int h, uint32_t *out)
{
    if (s_ready <= 0 || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > VW || y + h > VH) return 0;
    if (!s_capture) s_capture = linearAlloc((size_t)VW * VH * 4u);
    if (!s_capture) return 0;
    C3D_SyncDisplayTransfer((u32 *)s_vram.data, GX_BUFFER_DIM(VW, VH),
                            s_capture, GX_BUFFER_DIM(VW, VH),
                            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                            GX_TRANSFER_RAW_COPY(0) |
                            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB5A1) |
                            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(s_capture, (u32)VW * VH * 4u);
    /* The transfer writes framebuffer row 0 (VRAM row 0, see the projection)
     * as the LAST linear row. */
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            out[(size_t)r * w + c] = s_capture[(size_t)(VH - 1 - (y + r)) * VW + (x + c)] >> 8;
    return 1;
}

void n3ds_shadow_status(char *buf, size_t cap)
{
    /* citro3d's own timers: GPU time of the last frame (all passes) and the
     * CPU time spent building/submitting it. */
    const int n = snprintf(buf, cap, "gpu %.2f ms cpu %.2f ms | ",
                           (double)C3D_GetDrawingTime(), (double)C3D_GetProcessingTime());
    if (n < 0 || (size_t)n >= cap) return;
    buf += n; cap -= (size_t)n;
    snprintf(buf, cap, "tris=%lu (gouraud %lu) rects=%lu fills=%lu blend=%lu/%lu/%lu/%lu/%lu "
             "maskset=%lu textured=%lu lines=%lu copies=%lu uploads=%lu maskcheck=%lu dropped=%lu",
             (unsigned long)s_n.tris, (unsigned long)s_n.gouraud, (unsigned long)s_n.rects,
             (unsigned long)s_n.fills, (unsigned long)s_n.blend[0], (unsigned long)s_n.blend[1],
             (unsigned long)s_n.blend[2], (unsigned long)s_n.blend[3], (unsigned long)s_n.blend[4],
             (unsigned long)s_n.mask_set,
             (unsigned long)s_n.textured, (unsigned long)s_n.lines, (unsigned long)s_n.copies,
             (unsigned long)s_n.uploads, (unsigned long)s_n.mask_check, (unsigned long)s_n.dropped);
    memset(&s_n, 0, sizeof s_n);
}
