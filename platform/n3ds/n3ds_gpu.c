/* n3ds_gpu.c — see n3ds_gpu.h. */
#include "n3ds_gpu.h"

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>

#include "n3ds_gpu_shadow.h"
#include "present_shbin.h"   /* built from shaders/present.v.pica by picasso */

/* The PS1 picture keeps its 4:3 shape: 320x240 in the middle of the 400x240
 * top screen. Never stretched to 400 wide (true widescreen is a later,
 * separate feature that renders more of the scene, not a stretch). */
enum { SCREEN_W = 400, SCREEN_H = 240, OUT_W = 320, OUT_H = 240,
       OUT_X = (SCREEN_W - OUT_W) / 2 };

/* Render target RGBA8 -> the RGBA8 framebuffers SDL's 3DS video set up. */
#define DISPLAY_TRANSFER_FLAGS                                                   \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |                             \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |                            \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
/* Linear staging image -> tiled texture (the 3DS's usual upload flags). */
#define TEXTURE_TRANSFER_FLAGS                                                   \
    (GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |                             \
     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |                            \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static int               s_state;        /* 0 not tried, 1 ready, -1 failed */
static C3D_RenderTarget *s_top;
static DVLB_s           *s_dvlb;
static shaderProgram_s   s_program;
static int               s_proj_loc;
static C3D_Mtx           s_proj;
static C3D_Tex           s_tex;
static int               s_tex_w, s_tex_h;   /* 0 = no texture yet */
static uint32_t         *s_stage;            /* linear, s_tex_w x s_tex_h */
static uint32_t         *s_capture;          /* linear, 240 x 400 (rotated) */
static int               s_drawn;

static int pow2_at_least(int v, int min)
{
    int p = min;
    while (p < v) p <<= 1;
    return p;
}

static int gpu_init(void)
{
    if (gfxGetScreenFormat(GFX_TOP) != GSP_RGBA8_OES) {
        printf("thps2-n3ds: GPU present off: top screen is not RGBA8\n");
        return 0;
    }
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        printf("thps2-n3ds: GPU present off: C3D_Init failed\n");
        return 0;
    }
    s_top = C3D_RenderTargetCreate(SCREEN_H, SCREEN_W, GPU_RB_RGBA8, -1);
    if (!s_top) {
        printf("thps2-n3ds: GPU present off: no top render target (VRAM)\n");
        C3D_Fini();
        return 0;
    }
    C3D_RenderTargetSetOutput(s_top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    s_dvlb = DVLB_ParseFile((u32 *)present_shbin, present_shbin_size);
    shaderProgramInit(&s_program);
    shaderProgramSetVsh(&s_program, &s_dvlb->DVLE[0]);
    s_proj_loc = shaderInstanceGetUniformLocation(s_program.vertexShader, "projection");
    /* y down, origin at the top-left of the screen as the player sees it. */
    Mtx_OrthoTilt(&s_proj, 0.0f, (float)SCREEN_W, (float)SCREEN_H, 0.0f, 0.0f, 1.0f, true);
    printf("thps2-n3ds: GPU present on (citro3d), linear free %lu KB, VRAM free %lu KB\n",
           (unsigned long)(linearSpaceFree() >> 10), (unsigned long)(vramSpaceFree() >> 10));
    return 1;
}

/* Texture and staging image big enough for w x h (PS1 frames are at most
 * 640 x 480; 512 x 240 is THPS2's usual). */
static int ensure_texture(int w, int h)
{
    const int tw = pow2_at_least(w, 64), th = pow2_at_least(h, 64);
    if (tw == s_tex_w && th == s_tex_h) return 1;
    if (tw > 1024 || th > 1024) return 0;
    if (s_tex_w) { C3D_TexDelete(&s_tex); linearFree(s_stage); s_tex_w = s_tex_h = 0; }
    s_stage = linearAlloc((size_t)tw * th * 4u);
    if (!s_stage) return 0;
    if (!C3D_TexInitVRAM(&s_tex, (u16)tw, (u16)th, GPU_RGBA8)) {
        linearFree(s_stage); s_stage = NULL;
        return 0;
    }
    /* Nearest: the same pixels the CPU presenter picked (Phase 1 gate). */
    C3D_TexSetFilter(&s_tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&s_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    s_tex_w = tw; s_tex_h = th;
    return 1;
}

static void set_state(C3D_Tex *tex)
{
    C3D_BindProgram(&s_program);
    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);   /* v0 position */
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 4);   /* v1 texcoord */
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_proj_loc, &s_proj);
    C3D_TexBind(0, tex);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}

static void vertex(float x, float y, float u, float v)
{
    C3D_ImmSendAttrib(x, y, 0.5f, 1.0f);
    C3D_ImmSendAttrib(u, v, 0.0f, 1.0f);
}

static void draw_frame(C3D_Tex *tex, int tw, int th, int x, int y, int w, int h);

int n3ds_gpu_present(const uint32_t *argb, int w, int h, int pitch)
{
    if (s_state == 0) s_state = gpu_init() ? 1 : -1;
    if (s_state < 0 || !ensure_texture(w, h)) return 0;

    /* ARGB8888 -> the RGBA8 word order the transfer engine reads. */
    for (int y = 0; y < h; y++) {
        const uint32_t *src = argb + (size_t)y * (size_t)pitch;
        uint32_t *dst = s_stage + (size_t)y * (size_t)s_tex_w;
        for (int x = 0; x < w; x++) dst[x] = (src[x] << 8) | 0xFFu;
    }
    GSPGPU_FlushDataCache(s_stage, (u32)s_tex_w * (u32)h * 4u);
    C3D_SyncDisplayTransfer(s_stage, GX_BUFFER_DIM(s_tex_w, s_tex_h),
                            (u32 *)s_tex.data, GX_BUFFER_DIM(s_tex_w, s_tex_h),
                            TEXTURE_TRANSFER_FLAGS);

    draw_frame(&s_tex, s_tex_w, s_tex_h, 0, 0, w, h);
    return 1;
}

/* One frame on the top screen: the GPU renderer's batches (into its VRAM
 * copy), then texture rectangle [x, x+w) x [y, y+h) (texels of a tw x th
 * texture, row y at t = y / th) scaled to 320x240 in the middle of the
 * screen, black bars beside it. */
static void draw_frame(C3D_Tex *tex, int tw, int th, int x, int y, int w, int h)
{
    /* Nearest sampling happens at output pixel centres (x + 0.5). Shifting
     * the coordinates back half an output pixel (plus a tiny epsilon against
     * landing exactly on a texel edge) picks source column floor(x * w / 320)
     * and row floor(y * h / 240): the same pixels the CPU presenter chose.
     * 1/32 source pixel: well above the PICA's texture-coordinate precision
     * (1/1024 was lost and picked the column to the left), and below the
     * smallest step of x * w / 320 for PS1 widths (0.05 at 368). */
    const float eps = 1.0f / 32.0f;
    const float sx = (float)w / (float)OUT_W, sy = (float)h / (float)OUT_H;
    const float u0 = ((float)x - 0.5f * sx + eps) / (float)tw;
    const float u1 = ((float)x + ((float)OUT_W - 0.5f) * sx + eps) / (float)tw;
    const float v0 = ((float)y - 0.5f * sy + eps) / (float)th;
    const float v1 = ((float)y + ((float)OUT_H - 0.5f) * sy + eps) / (float)th;
    const float x0 = (float)OUT_X, x1 = (float)(OUT_X + OUT_W);

    /* No C3D_FRAME_SYNCDRAW: C3D_FrameSync always waits at least one vblank,
     * which below full speed only adds up to 16.7 ms to every frame. The
     * swap still happens at vblank, and the runtime's own wall-clock cap
     * (16.67 ms) keeps it from running faster than the PS1. Waits only if
     * the GPU is still drawing the previous frame. */
    C3D_FrameBegin(0);
    n3ds_shadow_render();   /* the GPU renderer's primitives for this frame */
    C3D_RenderTargetClear(s_top, C3D_CLEAR_ALL, 0x000000FF, 0);
    C3D_FrameDrawOn(s_top);
    set_state(tex);
    C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
    vertex(x0, 0.0f,          u0, v0);
    vertex(x0, (float)OUT_H,  u0, v1);
    vertex(x1, 0.0f,          u1, v0);
    vertex(x1, (float)OUT_H,  u1, v1);
    C3D_ImmDrawEnd();
    C3D_FrameEnd(0);
    s_drawn = 1;
}

int n3ds_gpu_present_texture(C3D_Tex *tex, int x, int y, int w, int h)
{
    if (s_state == 0) s_state = gpu_init() ? 1 : -1;
    if (s_state < 0) return 0;
    draw_frame(tex, tex->width, tex->height, x, y, w, h);
    return 1;
}

int n3ds_gpu_capture_top(uint32_t *out)
{
    if (s_state <= 0 || !s_drawn) return 0;
    if (!s_capture) s_capture = linearAlloc((size_t)SCREEN_W * SCREEN_H * 4u);
    if (!s_capture) return 0;
    /* The tiled render target -> linear, still in the panel's rotated layout. */
    C3D_SyncDisplayTransfer((u32 *)s_top->frameBuf.colorBuf, GX_BUFFER_DIM(SCREEN_H, SCREEN_W),
                            s_capture, GX_BUFFER_DIM(SCREEN_H, SCREEN_W),
                            DISPLAY_TRANSFER_FLAGS);
    GSPGPU_InvalidateDataCache(s_capture, (u32)SCREEN_W * SCREEN_H * 4u);
    /* Rotated: screen (x, y) lives at [x * 240 + (239 - y)], RGBA8 words. */
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++)
            out[(size_t)y * SCREEN_W + x] = s_capture[(size_t)x * SCREEN_H + (SCREEN_H - 1 - y)] >> 8;
    return 1;
}
