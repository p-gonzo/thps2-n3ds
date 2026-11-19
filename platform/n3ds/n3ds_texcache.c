/* n3ds_texcache.c — see n3ds_texcache.h. */
#include "n3ds_texcache.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu.h"   /* psxrecomp: gpu_get_vram (the authoritative CPU VRAM) */

enum {
    CELL_W = 64, CELL_H = 16, CELLS_X = 1024 / CELL_W, CELLS_Y = 512 / CELL_H,
    MAX_ENTRIES = 512,
    BUDGET_BYTES = 6 * 1024 * 1024,   /* decoded textures kept, linear memory */
    GRAVE = 256,
};

typedef struct {
    uint32_t key[3];            /* page/depth/clut, window shape, variant */
    C3D_Tex  tex;
    int      used;              /* slot in use */
    int      ever;              /* slot was used once: probing continues past it */
    uint32_t decoded_serial;    /* write serial the decode saw */
    uint32_t checked_serial;    /* last write serial it was validated against */
    uint32_t last_frame;
    uint16_t w, h;
    uint16_t ou, ov;            /* decoded block origin (texels; aligned to w, h) */
    uint8_t  grow_u, grow_v;    /* axis without a window: block grows to what is drawn */
    /* Source cells: the texel rectangle and the CLUT row. */
    uint8_t  sx1, sy1, sx2, sy2, cx1, cy, cx2, has_clut;
    float    su, sv;
} Entry;

static Entry    s_e[MAX_ENTRIES];
static uint32_t s_cell[CELLS_Y][CELLS_X];   /* last write serial per cell */
static uint32_t s_serial = 1;               /* bumped by every VRAM write */
static uint32_t s_frame;
static uint32_t s_bytes;
static struct { C3D_Tex tex; uint32_t frame; } s_grave[GRAVE];
static unsigned s_ngrave;
static struct { uint32_t hits, decodes, redecodes, grows, evictions, texels, fail; } s_n;

void texcache_note_write(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    s_serial++;
    /* Cells covering [x, x+w) x [y, y+h), wrapping at the VRAM edges. */
    const int x0 = x & 1023, y0 = y & 511;
    int ncx = (x0 % CELL_W + w + CELL_W - 1) / CELL_W, ncy = (y0 % CELL_H + h + CELL_H - 1) / CELL_H;
    if (ncx > CELLS_X) ncx = CELLS_X;
    if (ncy > CELLS_Y) ncy = CELLS_Y;
    for (int j = 0; j < ncy; j++)
        for (int i = 0; i < ncx; i++)
            s_cell[(y0 / CELL_H + j) % CELLS_Y][(x0 / CELL_W + i) % CELLS_X] = s_serial;
}

static uint32_t cells_max(int x1, int y1, int x2, int y2)
{
    uint32_t m = 0;
    for (int cy = y1; ; cy = (cy + 1) % CELLS_Y) {
        for (int cx = x1; ; cx = (cx + 1) % CELLS_X) {
            if (s_cell[cy][cx] > m) m = s_cell[cy][cx];
            if (cx == x2) break;
        }
        if (cy == y2) break;
    }
    return m;
}

static int entry_fresh(Entry *e)
{
    if (e->checked_serial == s_serial) return 1;
    uint32_t m = cells_max(e->sx1, e->sy1, e->sx2, e->sy2);
    if (e->has_clut) {
        const uint32_t c = cells_max(e->cx1, e->cy, e->cx2, e->cy);
        if (c > m) m = c;
    }
    e->checked_serial = s_serial;
    return m <= e->decoded_serial;
}

/* The GPU may still sample a texture drawn in the last two frames. */
static void bury(C3D_Tex *t)
{
    if (s_ngrave == GRAVE) { C3D_TexDelete(t); return; }   /* cannot happen at 2 frames */
    s_grave[s_ngrave].tex = *t;
    s_grave[s_ngrave].frame = s_frame;
    s_ngrave++;
}

static struct { C3D_Tex *tex; uint32_t frame; } s_retired[16];
static unsigned s_nretired;

void texcache_retire(C3D_Tex *t)
{
    if (s_nretired == 16) { C3D_TexDelete(s_retired[0].tex); free(s_retired[0].tex);
                            memmove(s_retired, s_retired + 1, 15 * sizeof s_retired[0]); s_nretired--; }
    s_retired[s_nretired].tex = t;
    s_retired[s_nretired].frame = s_frame;
    s_nretired++;
}

void texcache_frame(void)
{
    s_frame++;
    unsigned k = 0;
    for (unsigned i = 0; i < s_ngrave; i++) {
        if (s_frame - s_grave[i].frame >= 3) C3D_TexDelete(&s_grave[i].tex);
        else s_grave[k++] = s_grave[i];
    }
    s_ngrave = k;
    k = 0;
    for (unsigned i = 0; i < s_nretired; i++) {
        if (s_frame - s_retired[i].frame >= 3) { C3D_TexDelete(s_retired[i].tex); free(s_retired[i].tex); }
        else s_retired[k++] = s_retired[i];
    }
    s_nretired = k;
}

static void drop(Entry *e)
{
    s_bytes -= (uint32_t)e->w * e->h * 2u;
    bury(&e->tex);
    e->used = 0;
}

/* One axis of the PS1 texture window: the tile it repeats. A window maps
 * u -> (u & ~(mask*8)) | ((off & mask)*8); for the usual masks (contiguous
 * high bits) that is a tile of size 8 << ctz(mask) at origin (off & mask)*8,
 * which a repeating texture of that size reproduces exactly. */
typedef struct { int origin, size, general; } Axis;
static Axis window_axis(unsigned mask, unsigned off)
{
    Axis a = { 0, 256, 0 };
    if (!mask) return a;
    const unsigned low = (unsigned)__builtin_ctz(mask);
    if (mask != (0x1Fu & ~((1u << low) - 1u))) { a.general = 1; return a; }
    a.size = 8 << low;
    a.origin = (int)((off & mask) * 8u);
    return a;
}

/* Smallest power-of-two block (>= 8, <= 256), aligned to its size, holding
 * [lo, hi]. Aligned, so a repeating texture of that size samples texel
 * u - origin at s = u / size: no coordinate offset is needed. */
static void block_for(unsigned lo, unsigned hi, unsigned *origin, unsigned *size)
{
    unsigned sz = 8;
    while (sz < 256 && (lo / sz) != (hi / sz)) sz <<= 1;
    *size = sz;
    *origin = (lo / sz) * sz;
}

static inline unsigned morton8(unsigned x, unsigned y)
{
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

static inline uint16_t texel(const uint16_t *vram, unsigned depth, unsigned tpx, unsigned tpy,
                             unsigned cx, unsigned cy, unsigned u, unsigned v)
{
    const unsigned row = ((tpy + v) & 511u) * 1024u;
    if (depth == 0) {
        const uint16_t hw = vram[row + ((tpx + (u >> 2)) & 1023u)];
        return vram[cy * 1024u + ((cx + ((hw >> ((u & 3u) * 4u)) & 15u)) & 1023u)];
    }
    if (depth == 1) {
        const uint16_t hw = vram[row + ((tpx + (u >> 1)) & 1023u)];
        return vram[cy * 1024u + ((cx + ((hw >> ((u & 1u) * 8u)) & 255u)) & 1023u)];
    }
    return vram[row + ((tpx + u) & 1023u)];
}

static inline uint16_t to5551(uint16_t c, int variant)
{
    int a;
    if (variant == TEXV_OPAQUE) a = c != 0 && !(c & 0x8000);
    else if (variant == TEXV_STP) a = (c & 0x8000) != 0;
    else a = c != 0;
    return (uint16_t)(((c & 31u) << 11) | (((c >> 5) & 31u) << 6) | (((c >> 10) & 31u) << 1) | (unsigned)a);
}

static void decode(Entry *e, uint16_t tp, uint16_t cxp, uint16_t cyp, uint32_t twin, int variant,
                   Axis ax, Axis ay)
{
    const uint16_t *vram = gpu_get_vram();
    const unsigned depth = (tp >> 7) & 3u, tpx = (tp & 15u) * 64u, tpy = ((tp >> 4) & 1u) * 256u;
    const unsigned mx = twin & 31u, my = (twin >> 5) & 31u, ox = (twin >> 10) & 31u, oy = (twin >> 15) & 31u;
    uint16_t *dst = (uint16_t *)e->tex.data;
    const unsigned W = e->w, H = e->h;
    for (unsigned j = 0; j < H; j++) {
        unsigned v = (unsigned)ay.origin + j;
        if (ay.general) v = (j & ~(my * 8u)) | ((oy & my) * 8u);
        for (unsigned i = 0; i < W; i++) {
            unsigned u = (unsigned)ax.origin + i;
            if (ax.general) u = (i & ~(mx * 8u)) | ((ox & mx) * 8u);
            const uint16_t c = texel(vram, depth, tpx, tpy, cxp, cyp, u & 255u, v & 255u);
            /* Texture memory starts at t = 1 (the top), so texel row j
             * (sampled at t = j / H) is stored at memory row H - 1 - j. */
            const unsigned m = H - 1u - j;
            dst[((m >> 3) * (W >> 3) + (i >> 3)) * 64u + morton8(i & 7u, m & 7u)] = to5551(c, variant);
        }
    }
    C3D_TexFlush(&e->tex);
    e->decoded_serial = s_serial;
    e->checked_serial = s_serial;
    s_n.texels += W * H;
}

static uint32_t hash3(const uint32_t k[3])
{
    return (k[0] * 2654435761u) ^ (k[1] * 40503u) ^ (k[2] * 97u);
}

static Entry *find(const uint32_t key[3])
{
    for (unsigned n = 0, i = hash3(key) % MAX_ENTRIES; n < MAX_ENTRIES; n++, i = (i + 1) % MAX_ENTRIES) {
        Entry *c = &s_e[i];
        if (c->used && !memcmp(c->key, key, 3 * sizeof(uint32_t))) return c;
        if (!c->ever) return NULL;
    }
    return NULL;
}

static Entry *free_slot(const uint32_t key[3])
{
    for (unsigned n = 0, i = hash3(key) % MAX_ENTRIES; n < MAX_ENTRIES; n++, i = (i + 1) % MAX_ENTRIES)
        if (!s_e[i].used) return &s_e[i];
    return NULL;
}

/* The decoded blocks of an entry, as axes for decode(). */
static void entry_axes(const Entry *e, Axis *ax, Axis *ay)
{
    if (!ax->general) { ax->origin = e->ou; ax->size = e->w; }
    if (!ay->general) { ay->origin = e->ov; ay->size = e->h; }
}

static int place(const uint32_t key[3], uint16_t tp, uint16_t cx, uint16_t cy, uint32_t twin,
                 int variant, Axis ax, Axis ay, int grow_u, int grow_v, TexRef *out);

int texcache_get(uint16_t tp, uint16_t cx, uint16_t cy, uint32_t twin, int variant,
                 int umin, int vmin, int umax, int vmax, TexRef *out)
{
    const unsigned depth = (tp >> 7) & 3u;
    Axis ax = window_axis(twin & 31u, (twin >> 10) & 31u);
    Axis ay = window_axis((twin >> 5) & 31u, (twin >> 15) & 31u);
    /* Without a window, only the block of the page the primitive samples. */
    const int grow_u = !(twin & 31u), grow_v = !((twin >> 5) & 31u);
    if (umin < 0) umin = 0;
    if (vmin < 0) vmin = 0;
    if (umax > 255) umax = 255;
    if (vmax > 255) vmax = 255;
    const int has_clut = depth < 2;
    uint32_t key[3];
    key[0] = (uint32_t)(tp & 0x19Fu) | (has_clut ? ((uint32_t)(cx & 0x3F0u) << 5) | ((uint32_t)(cy & 511u) << 15) : 0u);
    key[1] = (ax.general || ay.general) ? (twin | 0x80000000u)
             : ((uint32_t)ax.origin | ((uint32_t)ax.size << 8) | ((uint32_t)ay.origin << 17) | ((uint32_t)ay.size << 25)) ^ 0x40000000u;
    key[2] = (uint32_t)variant;

    Entry *e = find(key);
    if (e) {
        e->last_frame = s_frame;
        if ((e->grow_u && ((unsigned)umin < e->ou || (unsigned)umax >= e->ou + e->w)) ||
            (e->grow_v && ((unsigned)vmin < e->ov || (unsigned)vmax >= e->ov + e->h))) {
            /* Reaches outside the decoded block: decode a block covering both. */
            unsigned lo, hi, o, sz;
            Axis nx = ax, ny = ay;
            if (e->grow_u) {
                lo = (unsigned)umin < e->ou ? (unsigned)umin : e->ou;
                hi = (unsigned)umax > e->ou + e->w - 1u ? (unsigned)umax : e->ou + e->w - 1u;
                block_for(lo, hi, &o, &sz); nx.origin = (int)o; nx.size = (int)sz;
            } else { nx.origin = e->ou; nx.size = e->w; }
            if (e->grow_v) {
                lo = (unsigned)vmin < e->ov ? (unsigned)vmin : e->ov;
                hi = (unsigned)vmax > e->ov + e->h - 1u ? (unsigned)vmax : e->ov + e->h - 1u;
                block_for(lo, hi, &o, &sz); ny.origin = (int)o; ny.size = (int)sz;
            } else { ny.origin = e->ov; ny.size = e->h; }
            drop(e);
            s_n.grows++;
            return place(key, tp, cx, cy, twin, variant, nx, ny, grow_u, grow_v, out);
        }
        if (!entry_fresh(e)) {
            /* Stale: decode into a new texture; the old one may be in flight. */
            C3D_Tex old = e->tex;
            if (!C3D_TexInit(&e->tex, e->w, e->h, GPU_RGBA5551)) { e->tex = old; s_n.fail++; return 0; }
            C3D_TexSetFilter(&e->tex, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&e->tex, GPU_REPEAT, GPU_REPEAT);
            bury(&old);
            entry_axes(e, &ax, &ay);
            decode(e, tp, cx, cy, twin, variant, ax, ay);
            s_n.redecodes++;
        } else {
            s_n.hits++;
        }
        out->tex = &e->tex; out->su = e->su; out->sv = e->sv;
        return 1;
    }

    if (grow_u) { unsigned o, sz; block_for((unsigned)umin, (unsigned)umax, &o, &sz); ax.origin = (int)o; ax.size = (int)sz; }
    if (grow_v) { unsigned o, sz; block_for((unsigned)vmin, (unsigned)vmax, &o, &sz); ay.origin = (int)o; ay.size = (int)sz; }
    return place(key, tp, cx, cy, twin, variant, ax, ay, grow_u, grow_v, out);
}

/* Decodes a new entry for key with the given blocks. */
static int place(const uint32_t key[3], uint16_t tp, uint16_t cx, uint16_t cy, uint32_t twin,
                 int variant, Axis ax, Axis ay, int grow_u, int grow_v, TexRef *out)
{
    const unsigned depth = (tp >> 7) & 3u;
    const int has_clut = depth < 2;
    /* Evict least recently used entries (not this frame's)
     * until it fits the budget. */
    const uint16_t W = (uint16_t)ax.size, H = (uint16_t)ay.size;
    const uint32_t bytes = (uint32_t)W * H * 2u;
    while (s_bytes + bytes > BUDGET_BYTES) {
        Entry *lru = NULL;
        for (unsigned i = 0; i < MAX_ENTRIES; i++)
            if (s_e[i].used && s_e[i].last_frame != s_frame && (!lru || s_e[i].last_frame < lru->last_frame))
                lru = &s_e[i];
        if (!lru) { s_n.fail++; return 0; }
        drop(lru);
        s_n.evictions++;
    }
    Entry *e = free_slot(key);
    if (!e) { s_n.fail++; return 0; }
    memset(e, 0, sizeof *e);
    e->ever = 1;
    if (!C3D_TexInit(&e->tex, W, H, GPU_RGBA5551)) { e->used = 0; s_n.fail++; return 0; }
    C3D_TexSetFilter(&e->tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&e->tex, GPU_REPEAT, GPU_REPEAT);
    memcpy(e->key, key, 3 * sizeof(uint32_t));
    e->used = 1;
    e->w = W; e->h = H;
    e->su = 1.0f / (float)W; e->sv = 1.0f / (float)H;
    e->ou = (uint16_t)(ax.general ? 0 : ax.origin); e->ov = (uint16_t)(ay.general ? 0 : ay.origin);
    e->grow_u = (uint8_t)grow_u; e->grow_v = (uint8_t)grow_v;
    e->last_frame = s_frame;
    s_bytes += bytes;

    /* Cells the decode reads: the page area (in VRAM halfwords) and CLUT row. */
    const unsigned tpx = (tp & 15u) * 64u, tpy = ((tp >> 4) & 1u) * 256u;
    const unsigned u0 = (ax.general ? 0u : (unsigned)ax.origin), uw = ax.general ? 256u : (unsigned)ax.size;
    const unsigned v0 = (ay.general ? 0u : (unsigned)ay.origin), vh = ay.general ? 256u : (unsigned)ay.size;
    const unsigned div = depth == 0 ? 4u : depth == 1 ? 2u : 1u;
    const unsigned x1 = (tpx + u0 / div) & 1023u, x2 = (tpx + (u0 + uw - 1u) / div) & 1023u;
    const unsigned y1 = (tpy + v0) & 511u, y2 = (tpy + v0 + vh - 1u) & 511u;
    e->sx1 = (uint8_t)(x1 / CELL_W); e->sx2 = (uint8_t)(x2 / CELL_W);
    e->sy1 = (uint8_t)(y1 / CELL_H); e->sy2 = (uint8_t)(y2 / CELL_H);
    e->has_clut = (uint8_t)has_clut;
    if (has_clut) {
        const unsigned n = depth == 0 ? 16u : 256u;
        e->cx1 = (uint8_t)((cx & 1023u) / CELL_W);
        e->cx2 = (uint8_t)(((cx + n - 1u) & 1023u) / CELL_W);
        e->cy = (uint8_t)((cy & 511u) / CELL_H);
    }
    decode(e, tp, cx, cy, twin, variant, ax, ay);
    s_n.decodes++;
    out->tex = &e->tex; out->su = e->su; out->sv = e->sv;
    return 1;
}

void texcache_status(char *buf, unsigned cap)
{
    snprintf(buf, cap, "texcache hits %lu decodes %lu redecodes %lu grows %lu evictions %lu texels %lu fail %lu, %lu KB",
             (unsigned long)s_n.hits, (unsigned long)s_n.decodes, (unsigned long)s_n.redecodes,
             (unsigned long)s_n.grows,
             (unsigned long)s_n.evictions, (unsigned long)s_n.texels, (unsigned long)s_n.fail,
             (unsigned long)(s_bytes >> 10));
    memset(&s_n, 0, sizeof s_n);
}
