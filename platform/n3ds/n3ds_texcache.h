/* n3ds_texcache.h — PS1 textures decoded for the PICA200 (GPU renderer
 * Phase 3, docs/N3DS_GPU_RENDERER.md).
 *
 * The PICA200 has no palette textures and no fragment shaders, so PS1 4-bit
 * and 8-bit CLUT textures (and 15-bit direct ones) are decoded on the CPU from
 * the authoritative CPU VRAM into RGBA5551 textures, cached by
 * (texture page, depth, CLUT, texture window, variant) and re-decoded when
 * the VRAM they came from is written. */
#ifndef N3DS_TEXCACHE_H
#define N3DS_TEXCACHE_H

#include <stdint.h>
#include <citro3d.h>

/* What a decoded texel's alpha bit means; PS1 0x0000 is always transparent. */
enum {
    TEXV_ALL = 0,     /* every non-transparent texel (non-semi primitives) */
    TEXV_OPAQUE = 1,  /* texels without the STP bit (semi pass 1, drawn opaque) */
    TEXV_STP = 2,     /* texels with the STP bit (semi pass 2, blended) */
};

typedef struct {
    C3D_Tex *tex;
    /* u/v (PS1 texel coordinates, 0..255) -> s/t: s = u * su; the texture
     * repeats, so a texture window's tile wraps exactly as on the PS1. */
    float su, sv;
} TexRef;

/* Finds or decodes the texture a primitive samples; [umin, umax] x [vmin, vmax]
 * are the texels it reads (without a texture window only that block of the
 * page is decoded). 0 if it cannot (no memory). */
int  texcache_get(uint16_t texpage, uint16_t clut_x, uint16_t clut_y, uint32_t twin,
                  int variant, int umin, int vmin, int umax, int vmax, TexRef *out);
/* The CPU VRAM rectangle was written: textures decoded from it are stale. */
void texcache_note_write(int x, int y, int w, int h);
/* Frees a texture (C3D_Tex allocated with malloc) once the GPU can no longer
 * be reading it. */
void texcache_retire(C3D_Tex *t);
/* Once per presented frame: frees textures the GPU can no longer be reading. */
void texcache_frame(void);
/* Counters since the last call, as text; resets them. */
void texcache_status(char *buf, unsigned cap);

#endif
