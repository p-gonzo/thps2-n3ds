/* n3ds_gpu_shadow.c — GPU renderer Phases 2-3 (docs/N3DS_GPU_RENDERER.md).
 *
 * Through psxrecomp's host tap (gr_set_host_tap) this file receives every
 * primitive the game draws and draws it with the PICA200 into a 1024x512
 * RGBA5551 copy of PS1 VRAM: in submission order (no depth test), the PS1's
 * draw area as scissor, its four semi-transparency modes as blend equations.
 * Textures come from n3ds_texcache.c (CLUT/15-bit decoded on the CPU from
 * the authoritative CPU VRAM). Textured primitives modulate texel x vertex
 * colour x 2; semi-transparent ones draw twice, texels without the STP bit
 * opaque and texels with it blended, as the PS1 decides per texel.
 *
 * Shadow mode (THPS2_GPU_SHADOW=1): the software renderer still draws and
 * presents everything, and this draws the same frame on the GPU, so the two
 * can be compared (gpu_<frame>.bmp vs snap_<frame>.bmp). */
#include "n3ds_gpu_shadow.h"

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "draw_shbin.h"     /* shaders/draw.v.pica */
#include "gpu_render.h"     /* psxrecomp: GpuRenderBackend, gr_set_host_tap */
#include "n3ds_texcache.h"
#include "n3ds_gpu.h"
#include "gpu.h"            /* psxrecomp: gpu_get_vram */

enum { VW = 1024, VH = 512, MAX_VERTS = 32768, MAX_BATCHES = 8192 };

/* Vertex: position in VRAM pixels, texture coordinates (s, t), colour bytes. */
typedef struct { float x, y, s, t; uint8_t r, g, b, a; } Vtx;   /* 20 bytes */

enum { TEV_COLOR = 0, TEV_MODULATE = 1, TEV_RAW = 2, TEV_COPY = 3 };
typedef struct {
    uint32_t first, count;
    C3D_Tex *tex;                   /* NULL: untextured */
    uint8_t  blend;                 /* 0 opaque, 1 + PS1 semi mode */
    uint8_t  tev;
    uint16_t sx1, sy1, sx2, sy2;    /* scissor, inclusive VRAM pixels */
} Batch;

static int       s_on = -1;         /* 0 off, 1 shadow (THPS2_GPU_SHADOW), 2 GPU (THPS2_GPU) */
static int       s_ready;           /* GPU objects exist */
static int       s_cleared;
static float     s_bias = 0.5f;     /* PS1 samples pixel (x, y) at integer coords */
static float     s_uv_eps = 1.0f / 32.0f;   /* texels: keeps exact u on its texel */
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
static uint32_t *s_capture;         /* linear VW x VH x 2 bytes, for snapshots */

/* PS1 drawing state as last set through the facade. */
static int s_semi, s_semi_mode, s_mask_set, s_mask_check;
static int s_ax1, s_ay1, s_ax2 = VW - 1, s_ay2 = VH - 1;
static uint32_t s_twin;
static int s_mod_r = 128, s_mod_g = 128, s_mod_b = 128, s_mod_raw;

/* Counters, reported once a second by the HUD status line. */
static struct {
    uint32_t tris, rects, fills, textured, lines, copies, uploads, mask_check, dropped;
    uint32_t blend[5];   /* primitives per mode: opaque, B/2+F/2, B+F, B-F, B+F/4 */
    uint32_t gouraud, tex_semi, tex_raw, tex_window, tex_fail, batches;
    uint32_t gpu_presents, cpu_presents, acquires, losses, stale_reads, owned_prims;
} s_n;

/* ---- Display-buffer ownership (THPS2_GPU=1) -------------------------------
 * A buffer the game displays is drawn by the GPU alone once its CPU VRAM
 * copy has been pushed to the GPU ("acquire"); the software renderer then
 * skips primitives into it (gr_set_host_draw_owner) and the presenter shows
 * it from the GPU copy. CPU writes into it (uploads, copies; FMV) give it
 * back to the software renderer, and a later primitive acquires it again
 * once two presents passed without such writes. */
typedef struct { int x, y, w, h, owned, used; uint32_t shown, cpu_write; } Buf;
enum { BUFS = 4 };
static Buf      s_buf[BUFS];
static uint32_t s_present_no = 1;

static int overlap(int ax1, int ay1, int ax2, int ay2, const Buf *b, long *area)
{
    const int x1 = ax1 > b->x ? ax1 : b->x, y1 = ay1 > b->y ? ay1 : b->y;
    const int x2 = ax2 < b->x + b->w - 1 ? ax2 : b->x + b->w - 1;
    const int y2 = ay2 < b->y + b->h - 1 ? ay2 : b->y + b->h - 1;
    if (x2 < x1 || y2 < y1) return 0;
    *area = (long)(x2 - x1 + 1) * (y2 - y1 + 1);
    return 1;
}

static void note_cpu_write(int x, int y, int w, int h)
{
    for (int i = 0; i < BUFS; i++) {
        Buf *b = &s_buf[i];
        long a;
        if (!b->used || !overlap(x, y, x + w - 1, y + h - 1, b, &a)) continue;
        b->cpu_write = s_present_no;
        if (b->owned) { b->owned = 0; s_n.losses++; }
    }
}

static void sync_up(const Buf *b);   /* below */

/* gr_set_host_draw_owner: does the primitive about to be drawn belong to the GPU? */
static int owns_draw(void)
{
    if (s_on != 2) return 0;
    const long area = (long)(s_ax2 - s_ax1 + 1) * (s_ay2 - s_ay1 + 1);
    for (int i = 0; i < BUFS; i++) {
        Buf *b = &s_buf[i];
        long a;
        if (!b->used || !overlap(s_ax1, s_ay1, s_ax2, s_ay2, b, &a) || a * 2 < area) continue;
        if (!b->owned) {
            if (s_present_no - b->cpu_write < 2) return 0;
            sync_up(b);
            b->owned = 1;
            s_n.acquires++;
        }
        s_n.owned_prims++;
        return 1;
    }
    return 0;
}

int n3ds_shadow_enabled(void)
{
    if (s_on < 0) {
        const char *g = getenv("THPS2_GPU");
        const char *e = getenv("THPS2_GPU_SHADOW");
        s_on = (g && g[0] == '1') ? 2 : (e && e[0] == '1') ? 1 : 0;
        const char *b = getenv("THPS2_GPU_BIAS");
        if (b) s_bias = strtof(b, NULL);
        const char *u = getenv("THPS2_GPU_UV_EPS");
        if (u) s_uv_eps = strtof(u, NULL);
    }
    return s_on;
}

/* ---- Recording (called on the game thread through the tap) --------------- */

static Batch *batch_for(int blend, int tev, C3D_Tex *tex)
{
    Batch *b = s_nbatches ? &s_batches[s_nbatches - 1] : NULL;
    if (b && b->blend == blend && b->tev == tev && b->tex == tex &&
        b->sx1 == s_ax1 && b->sy1 == s_ay1 && b->sx2 == s_ax2 && b->sy2 == s_ay2 &&
        b->first + b->count == s_nverts)
        return b;
    if (s_nbatches == MAX_BATCHES) return NULL;
    b = &s_batches[s_nbatches++];
    b->first = s_nverts; b->count = 0; b->blend = (uint8_t)blend; b->tev = (uint8_t)tev; b->tex = tex;
    b->sx1 = (uint16_t)s_ax1; b->sy1 = (uint16_t)s_ay1; b->sx2 = (uint16_t)s_ax2; b->sy2 = (uint16_t)s_ay2;
    return b;
}

static inline uint8_t c5to8(unsigned c) { return (uint8_t)((c << 3) | (c >> 2)); }

static Vtx *reserve(int n, int blend, int tev, C3D_Tex *tex)
{
    if (!s_vbo[s_cur] || s_nverts + (uint32_t)n > MAX_VERTS) { s_n.dropped++; return NULL; }
    Batch *b = batch_for(blend, tev, tex);
    if (!b) { s_n.dropped++; return NULL; }
    Vtx *v = &s_vbo[s_cur][s_nverts];
    s_nverts += (uint32_t)n;
    b->count += (uint32_t)n;
    return v;
}

static inline void put15(Vtx *v, float x, float y, uint16_t c, uint8_t a)
{
    v->x = x; v->y = y; v->s = 0.0f; v->t = 0.0f;
    v->r = c5to8(c & 31); v->g = c5to8((c >> 5) & 31); v->b = c5to8((c >> 10) & 31);
    v->a = a;
}

/* 24-bit PS1 colour 0x00BBGGRR (128 = texel x 1.0). */
static inline void put24(Vtx *v, float x, float y, float s, float t, uint32_t c)
{
    v->x = x; v->y = y; v->s = s; v->t = t;
    v->r = (uint8_t)c; v->g = (uint8_t)(c >> 8); v->b = (uint8_t)(c >> 16); v->a = 255;
}

static int cur_blend(void)
{
    const int b = s_semi ? 1 + (s_semi_mode & 3) : 0;
    s_n.blend[b]++;
    if (s_mask_check) s_n.mask_check++;
    return b;
}

static void tri(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1,
                int x2, int y2, uint16_t c2)
{
    Vtx *v = reserve(3, cur_blend(), TEV_COLOR, NULL);
    if (!v) return;
    const uint8_t a = s_mask_set ? 255 : 0;
    const float o = s_bias;
    put15(&v[0], (float)x0 + o, (float)y0 + o, c0, a);
    put15(&v[1], (float)x1 + o, (float)y1 + o, c1, a);
    put15(&v[2], (float)x2 + o, (float)y2 + o, c2, a);
}

/* Axis-aligned rectangle covering pixels [x, x+w) x [y, y+h): its edges sit on
 * pixel boundaries, so it needs no sampling bias. */
static void rect(int x, int y, int w, int h, uint16_t c, uint8_t a, int blend)
{
    Vtx *v = reserve(6, blend, TEV_COLOR, NULL);
    if (!v) return;
    const float l = (float)x, t = (float)y, r = (float)(x + w), b = (float)(y + h);
    put15(&v[0], l, t, c, a); put15(&v[1], r, t, c, a); put15(&v[2], l, b, c, a);
    put15(&v[3], r, t, c, a); put15(&v[4], r, b, c, a); put15(&v[5], l, b, c, a);
}

/* A textured primitive: one pass, or two for semi-transparency (texels
 * without STP opaque, then texels with STP blended, as the PS1 decides per
 * texel). Positions/UVs are prepared by the caller for n vertices. */
typedef struct { float x, y, u, v; uint32_t c; } TV;

static void textured(const TV *tv, int n, uint16_t tp, uint16_t cx, uint16_t cy, int raw)
{
    s_n.textured++;
    if (raw) s_n.tex_raw++;
    if (s_twin & 0x3FF) s_n.tex_window++;
    const int blend = cur_blend();
    const int tev = raw ? TEV_RAW : TEV_MODULATE;
    const int passes = blend ? 2 : 1;
    float fu0 = tv[0].u, fu1 = tv[0].u, fv0 = tv[0].v, fv1 = tv[0].v;
    for (int i = 1; i < n; i++) {
        if (tv[i].u < fu0) fu0 = tv[i].u; if (tv[i].u > fu1) fu1 = tv[i].u;
        if (tv[i].v < fv0) fv0 = tv[i].v; if (tv[i].v > fv1) fv1 = tv[i].v;
    }
    /* Texels read: up to the largest coordinate, which sprites give one past
     * their last texel. */
    const int umin = (int)fu0, vmin = (int)fv0;
    const int umax = n == 6 ? (int)fu1 - 1 : (int)fu1, vmax = n == 6 ? (int)fv1 - 1 : (int)fv1;
    if (blend) s_n.tex_semi++;
    for (int p = 0; p < passes; p++) {
        const int variant = !blend ? TEXV_ALL : p == 0 ? TEXV_OPAQUE : TEXV_STP;
        TexRef ref;
        if (!texcache_get(tp, cx, cy, s_twin, variant, umin, vmin, umax, vmax, &ref)) { s_n.tex_fail++; return; }
        Vtx *v = reserve(n, p == 0 ? 0 : blend, tev, ref.tex);
        if (!v) return;
        for (int i = 0; i < n; i++)
            put24(&v[i], tv[i].x, tv[i].y, (tv[i].u + s_uv_eps) * ref.su, (tv[i].v + s_uv_eps) * ref.sv,
                  tv[i].c);
    }
}

static uint32_t mod24(void)
{
    return (uint32_t)s_mod_r | ((uint32_t)s_mod_g << 8) | ((uint32_t)s_mod_b << 16);
}

static void t_set_semi(int e, int m) { s_semi = e; s_semi_mode = m; }
static void t_set_mask(int set, int check) { s_mask_set = set; s_mask_check = check; }
static void t_set_twin(uint32_t raw) { s_twin = raw; }
static void t_set_mod(int r, int g, int b, int raw) { s_mod_r = r; s_mod_g = g; s_mod_b = b; s_mod_raw = raw; }
static void t_set_area(int x1, int y1, int x2, int y2)
{
    s_ax1 = x1 < 0 ? 0 : x1 > VW - 1 ? VW - 1 : x1;
    s_ay1 = y1 < 0 ? 0 : y1 > VH - 1 ? VH - 1 : y1;
    s_ax2 = x2 < 0 ? 0 : x2 > VW - 1 ? VW - 1 : x2;
    s_ay2 = y2 < 0 ? 0 : y2 > VH - 1 ? VH - 1 : y2;
}

/* Every write the game makes to VRAM may change a texture. */
static void wrote_tri(int x0, int y0, int x1, int y1, int x2, int y2)
{
    int xl = x0, xh = x0, yl = y0, yh = y0;
    if (x1 < xl) xl = x1; if (x2 < xl) xl = x2; if (x1 > xh) xh = x1; if (x2 > xh) xh = x2;
    if (y1 < yl) yl = y1; if (y2 < yl) yl = y2; if (y1 > yh) yh = y1; if (y2 > yh) yh = y2;
    if (xl < s_ax1) xl = s_ax1; if (yl < s_ay1) yl = s_ay1;
    if (xh > s_ax2) xh = s_ax2; if (yh > s_ay2) yh = s_ay2;
    if (xl <= xh && yl <= yh) texcache_note_write(xl, yl, xh - xl + 1, yh - yl + 1);
}
static void wrote_rect(int x, int y, int w, int h)
{
    int xl = x, yl = y, xh = x + w - 1, yh = y + h - 1;
    if (xl < s_ax1) xl = s_ax1; if (yl < s_ay1) yl = s_ay1;
    if (xh > s_ax2) xh = s_ax2; if (yh > s_ay2) yh = s_ay2;
    if (xl <= xh && yl <= yh) texcache_note_write(xl, yl, xh - xl + 1, yh - yl + 1);
}

static void t_flat_tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c)
{ s_n.tris++; tri(x0, y0, c, x1, y1, c, x2, y2, c); wrote_tri(x0, y0, x1, y1, x2, y2); }
static void t_gouraud_tri(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1,
                          int x2, int y2, uint16_t c2)
{ s_n.tris++; s_n.gouraud++; tri(x0, y0, c0, x1, y1, c1, x2, y2, c2); wrote_tri(x0, y0, x1, y1, x2, y2); }
static void t_flat_rect(int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) return;
    s_n.rects++;
    rect(x, y, w, h, c, s_mask_set ? 255 : 0, cur_blend());
    wrote_rect(x, y, w, h);
}
/* GP0(02h): ignores draw area, mask and blending; the colour's bit 15 is
 * written as is. Wrapping fills (past the VRAM edge) are rare: counted as
 * dropped rather than split. */
static void t_fill(int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) return;
    s_n.fills++;
    texcache_note_write(x, y, w, h);
    if (x < 0 || y < 0 || x + w > VW || y + h > VH) { s_n.dropped++; return; }
    const int ax1 = s_ax1, ay1 = s_ay1, ax2 = s_ax2, ay2 = s_ay2;
    s_ax1 = 0; s_ay1 = 0; s_ax2 = VW - 1; s_ay2 = VH - 1;
    rect(x, y, w, h, c, (c & 0x8000) ? 255 : 0, 0);
    s_ax1 = ax1; s_ay1 = ay1; s_ax2 = ax2; s_ay2 = ay2;
}
static void t_copy(int sx, int sy, int dx, int dy, int w, int h)
{
    if (sx == dx && sy == dy) return;   /* THPS2's per-frame 2x1 self-copy */
    texcache_note_write(dx, dy, w, h);
    /* Reading a GPU-owned buffer from CPU VRAM sees stale pixels until the
     * readback path exists (Phase 4); THPS2 does it twice in the menus. */
    for (int i = 0; i < BUFS; i++) {
        long a;
        if (s_buf[i].used && s_buf[i].owned && overlap(sx, sy, sx + w - 1, sy + h - 1, &s_buf[i], &a))
            s_n.stale_reads++;
    }
    note_cpu_write(dx, dy, w, h);
    s_n.copies++;                      /* not mirrored on the GPU yet (Phase 4) */
}
/* Lines stay with the software renderer (THPS2 draws none). */
static void t_line(int x0, int y0, int x1, int y1, uint16_t c)
{
    (void)c; s_n.lines++; wrote_tri(x0, y0, x1, y1, x1, y1);
    note_cpu_write(s_ax1, s_ay1, s_ax2 - s_ax1 + 1, s_ay2 - s_ay1 + 1);
}
static void t_shaded_line(int x0, int y0, uint16_t c0, int x1, int y1, uint16_t c1)
{
    (void)c0; (void)c1; s_n.lines++; wrote_tri(x0, y0, x1, y1, x1, y1);
    note_cpu_write(s_ax1, s_ay1, s_ax2 - s_ax1 + 1, s_ay2 - s_ay1 + 1);
}

static void t_tex_tri(int x0, int y0, int u0, int v0, int x1, int y1, int u1, int v1,
                      int x2, int y2, int u2, int v2, uint16_t cx, uint16_t cy, uint16_t tp)
{
    const float o = s_bias;
    const uint32_t c = mod24();
    const TV tv[3] = { { x0 + o, y0 + o, (float)u0, (float)v0, c },
                       { x1 + o, y1 + o, (float)u1, (float)v1, c },
                       { x2 + o, y2 + o, (float)u2, (float)v2, c } };
    textured(tv, 3, tp, cx, cy, s_mod_raw);
    wrote_tri(x0, y0, x1, y1, x2, y2);
}
static void t_shaded_tex_tri(int x0, int y0, int u0, int v0, uint32_t c0,
                             int x1, int y1, int u1, int v1, uint32_t c1,
                             int x2, int y2, int u2, int v2, uint32_t c2,
                             uint16_t cx, uint16_t cy, uint16_t tp, int raw)
{
    const float o = s_bias;
    const TV tv[3] = { { x0 + o, y0 + o, (float)u0, (float)v0, c0 },
                       { x1 + o, y1 + o, (float)u1, (float)v1, c1 },
                       { x2 + o, y2 + o, (float)u2, (float)v2, c2 } };
    textured(tv, 3, tp, cx, cy, raw);
    wrote_tri(x0, y0, x1, y1, x2, y2);
}
/* Sprite: pixel (x+i, y+j) shows texel (u+i, v+j); corners on pixel edges. */
static void sprite(int x, int y, int w, int h, float u0, float v0, float u1, float v1,
                   uint16_t cx, uint16_t cy, uint16_t tp)
{
    if (w <= 0 || h <= 0) return;
    const uint32_t c = mod24();
    const float l = (float)x, t = (float)y, r = (float)(x + w), b = (float)(y + h);
    const TV tv[6] = { { l, t, u0, v0, c }, { r, t, u1, v0, c }, { l, b, u0, v1, c },
                       { r, t, u1, v0, c }, { r, b, u1, v1, c }, { l, b, u0, v1, c } };
    textured(tv, 6, tp, cx, cy, s_mod_raw);
    wrote_rect(x, y, w, h);
}
static void t_tex_rect(int x, int y, int w, int h, int u, int v, uint16_t cx, uint16_t cy,
                       uint16_t tp)
{ sprite(x, y, w, h, (float)u, (float)v, (float)(u + w), (float)(v + h), cx, cy, tp); }
static void t_tex_rect_scaled(int x, int y, int w, int h, int u0, int v0, int u1, int v1,
                              uint16_t cx, uint16_t cy, uint16_t tp)
{ sprite(x, y, w, h, (float)u0, (float)v0, (float)u1, (float)v1, cx, cy, tp); }
static void t_upload(int x, int y, int w, int h, const uint16_t *d)
{ (void)d; s_n.uploads++; texcache_note_write(x, y, w, h); note_cpu_write(x, y, w, h); }

static const GpuRenderBackend k_tap = {
    .name = "n3ds-gpu",
    .set_semi_transparency = t_set_semi,
    .set_mask_bits = t_set_mask,
    .set_draw_area = t_set_area,
    .set_texture_window = t_set_twin,
    .set_color_modulation = t_set_mod,
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
    s_capture = linearAlloc((size_t)VW * VH * 2u);   /* snapshots; reserved before textures fill memory */
    gr_set_host_tap(&k_tap);
    if (s_on == 2) gr_set_host_draw_owner(owns_draw);
    printf("thps2-n3ds: GPU renderer on, %s (bias %.2f, uv eps %.4f)\n",
           s_on == 2 ? "presenting" : "shadow beside the software renderer",
           (double)s_bias, (double)s_uv_eps);
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

static void set_tev(int tev)
{
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    if (tev == TEV_COLOR) {
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        return;
    }
    if (tev == TEV_COPY) {
        /* CPU VRAM pushed to the GPU copy: texels as they are, mask bit included. */
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        return;
    }
    if (tev == TEV_MODULATE) {
        /* PS1: texel * colour / 128, clamped: (texel * colour / 255) * 2. */
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
    } else {
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, 0, 0);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    }
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    /* Texels the decode marked absent (alpha 0) are not drawn. */
    C3D_AlphaTest(true, GPU_GREATER, 0);
}

void n3ds_shadow_render(void)
{
    texcache_frame();
    if (!n3ds_shadow_enabled() || !gpu_objects()) { s_nverts = s_nbatches = 0; return; }
    if (!s_cleared) { C3D_RenderTargetClear(s_target, C3D_CLEAR_ALL, 0, 0); s_cleared = 1; }
    if (!s_nbatches) return;
    s_n.batches += s_nbatches;
    GSPGPU_FlushDataCache(s_vbo[s_cur], s_nverts * sizeof(Vtx));
    C3D_FrameDrawOn(s_target);
    C3D_BindProgram(&s_prog);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_proj_loc, &s_proj);
    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 2);          /* x, y */
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);          /* s, t */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);  /* r, g, b, a */
    C3D_BufInfo *bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, s_vbo[s_cur], sizeof(Vtx), 3, 0x210);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
    C3D_CullFace(GPU_CULL_NONE);
    int tev = -1, blend = -1;
    C3D_Tex *tex = NULL;
    for (uint32_t i = 0; i < s_nbatches; i++) {
        const Batch *b = &s_batches[i];
        if (!b->count) continue;
        if (b->tev != tev) { set_tev(b->tev); tev = b->tev; }
        if (b->blend != blend) { set_blend(b->blend); blend = b->blend; }
        if (b->tex && b->tex != tex) { C3D_TexBind(0, b->tex); tex = b->tex; }
        C3D_SetScissor(GPU_SCISSOR_NORMAL, b->sx1, b->sy1, b->sx2 + 1u, b->sy2 + 1u);
        C3D_DrawArrays(GPU_TRIANGLES, (int)b->first, (int)b->count);
    }
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    s_cur ^= 1;
    s_nverts = s_nbatches = 0;
}

/* Morton order inside an 8x8 tile: x0 y0 x1 y1 x2 y2. */
static inline unsigned morton8(unsigned x, unsigned y)
{
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

/* Snapshot readback. A plain DMA copy of the tiled RGBA5551 target (no GX
 * format conversion: the 1024x512 RGB5A1->RGBA8 display transfer that worked
 * in Azahar froze a real New 3DS), then detiled on the CPU. In memory, tile
 * strip 0 holds VRAM rows 511..504 (see the projection). */
int n3ds_shadow_capture(int x, int y, int w, int h, uint32_t *out)
{
    if (s_ready <= 0 || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > VW || y + h > VH) return 0;
    if (!s_capture) s_capture = linearAlloc((size_t)VW * VH * 2u);
    if (!s_capture) return 0;
    C3D_SyncTextureCopy((u32 *)s_vram.data, 0, s_capture, 0, (u32)VW * VH * 2u,
                        GX_TRANSFER_RAW_COPY(1));
    GSPGPU_InvalidateDataCache(s_capture, (u32)VW * VH * 2u);
    const uint16_t *px = (const uint16_t *)s_capture;
    for (int r = 0; r < h; r++) {
        const unsigned ly = (unsigned)(VH - 1 - (y + r));
        for (int c = 0; c < w; c++) {
            const unsigned lx = (unsigned)(x + c);
            const uint16_t p = px[((ly >> 3) * (VW >> 3) + (lx >> 3)) * 64u + morton8(lx & 7, ly & 7)];
            const unsigned r5 = (p >> 11) & 31, g5 = (p >> 6) & 31, b5 = (p >> 1) & 31;
            out[(size_t)r * w + c] = ((uint32_t)c5to8(r5) << 16) | ((uint32_t)c5to8(g5) << 8) | c5to8(b5);
        }
    }
    return 1;
}

void n3ds_shadow_status(char *buf, size_t cap)
{
    /* citro3d's own timers: GPU time of the last frame (all passes) and the
     * CPU time spent building/submitting it. */
    int n = snprintf(buf, cap, "gpu %.2f ms cpu %.2f ms | textured %lu (semi %lu raw %lu window %lu fail %lu) "
                     "batches %lu | ",
                     (double)C3D_GetDrawingTime(), (double)C3D_GetProcessingTime(),
                     (unsigned long)s_n.textured, (unsigned long)s_n.tex_semi,
                     (unsigned long)s_n.tex_raw, (unsigned long)s_n.tex_window,
                     (unsigned long)s_n.tex_fail, (unsigned long)s_n.batches);
    if (n < 0 || (size_t)n >= cap) return;
    buf += n; cap -= (size_t)n;
    n = snprintf(buf, cap, "tris=%lu (gouraud %lu) rects=%lu fills=%lu blend=%lu/%lu/%lu/%lu/%lu "
                 "lines=%lu copies=%lu uploads=%lu maskcheck=%lu dropped=%lu | present gpu %lu cpu %lu "
                 "acquire %lu lose %lu stale-read %lu owned-prims %lu | ",
                 (unsigned long)s_n.tris, (unsigned long)s_n.gouraud, (unsigned long)s_n.rects,
                 (unsigned long)s_n.fills, (unsigned long)s_n.blend[0], (unsigned long)s_n.blend[1],
                 (unsigned long)s_n.blend[2], (unsigned long)s_n.blend[3], (unsigned long)s_n.blend[4],
                 (unsigned long)s_n.lines, (unsigned long)s_n.copies, (unsigned long)s_n.uploads,
                 (unsigned long)s_n.mask_check, (unsigned long)s_n.dropped,
                 (unsigned long)s_n.gpu_presents, (unsigned long)s_n.cpu_presents,
                 (unsigned long)s_n.acquires, (unsigned long)s_n.losses,
                 (unsigned long)s_n.stale_reads, (unsigned long)s_n.owned_prims);
    if (n < 0 || (size_t)n >= cap) return;
    texcache_status(buf + n, (unsigned)(cap - (size_t)n));
    memset(&s_n, 0, sizeof s_n);
}

/* Pushes the CPU VRAM copy of a buffer into the GPU copy, in drawing order
 * (a batch), before the GPU takes it over. */
static void sync_up(const Buf *b)
{
    int W = 8, H = 8;
    while (W < b->w) W <<= 1;
    while (H < b->h) H <<= 1;
    C3D_Tex *t = malloc(sizeof *t);
    if (!t || !C3D_TexInit(t, (u16)W, (u16)H, GPU_RGBA5551)) { free(t); s_n.tex_fail++; return; }
    C3D_TexSetFilter(t, GPU_NEAREST, GPU_NEAREST);
    const uint16_t *vram = gpu_get_vram();
    uint16_t *dst = (uint16_t *)t->data;
    for (int j = 0; j < b->h; j++) {
        const unsigned m = (unsigned)(H - 1 - j);
        const uint16_t *row = vram + (size_t)((b->y + j) & 511) * 1024u;
        for (int i = 0; i < b->w; i++) {
            const uint16_t c = row[(b->x + i) & 1023];
            dst[((m >> 3) * ((unsigned)W >> 3) + ((unsigned)i >> 3)) * 64u + morton8((unsigned)i & 7, m & 7)] =
                (uint16_t)(((c & 31u) << 11) | (((c >> 5) & 31u) << 6) | (((c >> 10) & 31u) << 1) | (c >> 15));
        }
    }
    C3D_TexFlush(t);
    texcache_retire(t);   /* freed once the GPU is done with it */
    const int ax1 = s_ax1, ay1 = s_ay1, ax2 = s_ax2, ay2 = s_ay2;
    s_ax1 = 0; s_ay1 = 0; s_ax2 = VW - 1; s_ay2 = VH - 1;
    Vtx *v = reserve(6, 0, TEV_COPY, t);
    s_ax1 = ax1; s_ay1 = ay1; s_ax2 = ax2; s_ay2 = ay2;
    if (!v) return;
    const float l = (float)b->x, tp = (float)b->y, r = (float)(b->x + b->w), bt = (float)(b->y + b->h);
    const float s1 = (float)b->w / (float)W, t1 = (float)b->h / (float)H;
    const uint32_t c = 0x808080u;
    put24(&v[0], l, tp, 0, 0, c);   put24(&v[1], r, tp, s1, 0, c); put24(&v[2], l, bt, 0, t1, c);
    put24(&v[3], r, tp, s1, 0, c);  put24(&v[4], r, bt, s1, t1, c); put24(&v[5], l, bt, 0, t1, c);
}

int n3ds_shadow_present_vram(int x, int y, int w, int h, int depth24)
{
    s_present_no++;
    if (s_on != 2) return 0;
    Buf *b = NULL, *slot = NULL;
    for (int i = 0; i < BUFS; i++)
        if (s_buf[i].used && s_buf[i].x == x && s_buf[i].y == y && s_buf[i].w == w && s_buf[i].h == h)
            b = &s_buf[i];
    if (!b) {
        /* A free slot, else the buffer shown longest ago. */
        for (int i = 0; i < BUFS; i++) {
            if (!s_buf[i].used) { slot = &s_buf[i]; break; }
            if (!slot || s_buf[i].shown < slot->shown) slot = &s_buf[i];
        }
        b = slot;
        memset(b, 0, sizeof *b);
        b->used = 1; b->x = x; b->y = y; b->w = w; b->h = h;
        b->cpu_write = s_present_no;   /* acquired by the next primitive after two presents */
    }
    b->shown = s_present_no;
    if (depth24 || !b->owned || !gpu_objects()) { s_n.cpu_presents++; return 0; }
    s_n.gpu_presents++;
    return n3ds_gpu_present_texture(&s_vram, x, y, w, h);
}

int n3ds_shadow_mode(void) { n3ds_shadow_enabled(); return s_on; }
