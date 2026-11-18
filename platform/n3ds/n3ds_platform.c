/* n3ds_platform.c - New Nintendo 3DS process setup for the THPS2 recompilation.
 *
 * Built only for NINTENDO_3DS (CMakeLists.txt). Three things the desktop gets
 * from its OS and a .3dsx does not:
 *
 *  1. A large enough main-thread stack. libctru gives a .3dsx 32 KB unless
 *     __stacksize__ says otherwise; the recompiled BIOS and game nest deeply
 *     and the desktop runs them on an 8 MB stack. 1 MB is the starting point;
 *     PS1 threads get their own fiber stacks (psx_fiber.c).
 *
 *  2. A working directory. The runtime resolves game.toml, bios/, disc/ and
 *     saves relative to the current directory, the way it runs from
 *     build-release/ on a PC. A .3dsx starts wherever the loader leaves it, so
 *     move into the data folder (THPS2_N3DS_DATA_DIR, default sdmc:/3ds/thps2)
 *     before main. libctru mounts sdmc: before running constructors.
 *
 *  3. Somewhere for stdout/stderr to go. The runtime reports its startup and
 *     failures on stdout, as on a PC. Here both go to svcOutputDebugString:
 *     emulators log it (Azahar: Debug.Emulated) and a debugger can read it on
 *     hardware; with neither attached the call returns immediately.
 *
 * The runtime is built with the toolchain's default enum size, the same as
 * libctru, so libctru headers are safe to include here. (CTR-3D's game code
 * needs -fno-short-enums; this project does not.)
 */
#include <3ds.h>
#include <sys/iosupport.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unwind.h>   /* _Unwind_Ptr */

#include "host_present.h"   /* psxrecomp: psx_host_present_register */

#ifndef THPS2_N3DS_DATA_DIR
#define THPS2_N3DS_DATA_DIR "sdmc:/3ds/thps2"
#endif

u32 __stacksize__ = 1u * 1024u * 1024u;

/* C++ exception index lookup, correcting devkitARM's 3dsx.ld.
 *
 * The ARM unwinder finds a frame's handler through the .ARM.exidx table,
 * bracketed by __exidx_start / __exidx_end. 3dsx.ld sets
 *     __exidx_start = .;
 *     .ARM.exidx : { ... }
 * outside the section, so when .ARM.extab ends off a 4-byte boundary the
 * symbol lands on the alignment padding (this build: 0x0175f1fa for a table
 * at 0x0175f1fc). Every 8-byte entry is then read 2 bytes early, no handler is
 * ever found, and any throw - even one caught a frame up - ends in
 * std::terminate.
 *
 * libgcc asks __gnu_Unwind_Find_exidx (a weak hook) for the table when one is
 * defined. The table is 4-byte aligned and the padding is under 4 bytes, so
 * rounding the start up to 4 recovers the true start exactly (and is a no-op
 * when there is no padding). */
extern const char __exidx_start[];
extern const char __exidx_end[];

_Unwind_Ptr __gnu_Unwind_Find_exidx(_Unwind_Ptr pc, int *nrec)
{
    (void)pc;
    const uintptr_t start = ((uintptr_t)__exidx_start + 3u) & ~(uintptr_t)3u;
    *nrec = (int)(((uintptr_t)__exidx_end - start) / 8u);
    return (_Unwind_Ptr)start;
}

/* libctru's default linear heap is 32 MB. The New 3DS gives an application
 * 124 MB and this program's code and static data take about 97 MB, so the
 * default split leaves nothing for the normal heap and libctru stops before
 * main. Linear memory is only for GPU/DSP-visible buffers: SDL's framebuffers
 * (about 1.3 MB for both screens) and audio. The rest becomes the normal heap. */
u32 __ctru_linear_heap_size = 8u * 1024u * 1024u;

/* Frame presenter (psxrecomp host_present.h). The runtime's software path
 * would otherwise go through SDL's software renderer: a texture upload, a
 * scaled copy onto a 640x480 logical canvas, then SDL's 3DS driver copying
 * the window surface into the framebuffer again - three full-frame CPU passes
 * through generic per-pixel blitters. This writes the finished frame straight
 * into the top-screen framebuffer in one pass.
 *
 * The top screen is 400x240, stored rotated: screen pixel (x, y) is at
 * fb[x * 240 + (239 - y)]. A 4:3 PS1 frame fills a centred 320x240 area, so
 * the common 320x240 mode copies 1:1; other modes (256/368/512/640 wide,
 * 480i) are sampled nearest-neighbour into it. Only the RGBA8 format SDL
 * sets up is handled; anything else declines and SDL presents as before. */
static int n3ds_present_top(const uint32_t *argb, int w, int h, int pitch, int pin_43)
{
    (void)pin_43;   /* no widescreen yet: every frame is shown 4:3 */
    if (!argb || w <= 0 || h <= 0 || pitch < w) return 0;
    if (gfxGetScreenFormat(GFX_TOP) != GSP_RGBA8_OES) return 0;
    u16 rows = 0, cols = 0;   /* 240 x 400: the framebuffer is rotated */
    u32 *fb = (u32 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &rows, &cols);
    if (!fb || rows != 240 || cols < 320) return 0;

    enum { OUT_W = 320, OUT_H = 240 };
    const int x0 = (cols - OUT_W) / 2;
    static int src_row[OUT_H], src_row_h = -1;
    if (src_row_h != h) {
        for (int y = 0; y < OUT_H; y++) src_row[y] = (y * h) / OUT_H;
        src_row_h = h;
    }
    memset(fb, 0, (size_t)x0 * rows * 4u);
    memset(fb + (size_t)(x0 + OUT_W) * rows, 0, (size_t)(cols - x0 - OUT_W) * rows * 4u);
    for (int x = 0; x < OUT_W; x++) {
        const uint32_t *src = argb + (x * w) / OUT_W;
        u32 *dst = fb + (size_t)(x0 + x) * rows + (rows - 1);
        for (int y = 0; y < OUT_H; y++)
            dst[-y] = (src[(size_t)src_row[y] * (size_t)pitch] << 8) | 0xFFu;  /* ARGB -> RGBA */
    }
    GSPGPU_FlushDataCache(fb, (u32)rows * cols * 4u);
    gfxScreenSwapBuffers(GFX_TOP, false);
    gspWaitForVBlank();   /* the driver vsync the SDL path would have waited on */
    return 1;
}

/* One line at process exit, so a run that ends without a message from the
 * runtime still shows that it ended normally rather than crashed. */
static void thps2_n3ds_note_exit(void)
{
    static const char bye[] = "thps2-n3ds: process exit";
    svcOutputDebugString(bye, sizeof(bye) - 1);
}

__attribute__((constructor))
static void thps2_n3ds_process_setup(void)
{
    consoleDebugInit(debugDevice_SVC);            /* stderr -> svcOutputDebugString */
    devoptab_list[STD_OUT] = devoptab_list[STD_ERR];
    /* Not a terminal, so newlib would fully buffer: a hang or crash would
     * then lose exactly the lines that explain it. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    static const char hello[] = "thps2-n3ds: process setup done";
    svcOutputDebugString(hello, sizeof(hello) - 1);
    printf("thps2-n3ds: stdout reaches the debug log\n");
    atexit(thps2_n3ds_note_exit);
    psx_host_present_register(n3ds_present_top);
    /* If the folder is missing, the runtime reports the missing game.toml /
     * BIOS / disc on its own; nothing to add here. */
    (void)chdir(THPS2_N3DS_DATA_DIR);

    /* Opt-in: psx-env.txt in the data folder sets environment variables, one
     * KEY=VALUE per line ('#' starts a comment). A 3DS app has no shell, and
     * the runtime's PSX_* switches (PSX_FPS_TELEMETRY=1, ...) are read from
     * the environment, as on a PC. */
    FILE *env = fopen("psx-env.txt", "r");
    if (env) {
        char line[256];
        while (fgets(line, sizeof line, env)) {
            char *end = line + strlen(line);
            while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
                *--end = '\0';
            char *eq = strchr(line, '=');
            if (line[0] == '#' || !eq || eq == line) continue;
            *eq = '\0';
            setenv(line, eq + 1, 1);
            printf("thps2-n3ds: env %s=%s\n", line, eq + 1);
        }
        fclose(env);
    }

    /* Opt-in: an empty file named wait-for-debugger in the data folder holds
     * the process here for 15 s so a debugger (Azahar's GDB stub, or GDB over
     * Luma3DS on hardware) can attach before anything else runs. */
    if (access("wait-for-debugger", F_OK) == 0) {
        static const char wait[] = "thps2-n3ds: waiting 15 s for a debugger";
        svcOutputDebugString(wait, sizeof(wait) - 1);
        svcSleepThread(15LL * 1000 * 1000 * 1000);
    }
}
