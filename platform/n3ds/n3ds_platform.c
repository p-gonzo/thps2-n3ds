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
#include "n3ds_gpu.h"

extern uint64_t s_frame_count;   /* psxrecomp: guest frames since boot */
#include "n3ds_hud.h"

#ifndef THPS2_N3DS_BUILD
#define THPS2_N3DS_BUILD "unknown"
#endif
#ifndef THPS2_N3DS_PSXRECOMP
#define THPS2_N3DS_PSXRECOMP "unknown"
#endif

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
static int snapshot_maybe(const uint32_t *argb, int w, int h, int pitch);   /* below */
static void screen_snapshot_write(const uint32_t *rotated_rgba, uint32_t frame);
static uint32_t s_screen_snap_frame;   /* nonzero: also save the top screen */

/* Original CPU presenter: nearest-neighbour into the rotated framebuffer.
 * Kept as the fallback and for A/B checks (THPS2_PRESENT=cpu). */
static int present_top_cpu(const uint32_t *argb, int w, int h, int pitch)
{
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
    if (s_screen_snap_frame) {
        screen_snapshot_write(fb, s_screen_snap_frame);
        s_screen_snap_frame = 0;
    }
    gfxScreenSwapBuffers(GFX_TOP, false);
    gspWaitForVBlank();   /* the driver vsync the SDL path would have waited on */
    return 1;
}

/* Every PS1 frame is shown at 4:3 (320x240 centred on the 400x240 screen),
 * by the GPU (n3ds_gpu.c) unless THPS2_PRESENT=cpu or citro3d is unavailable.
 * pin_43 is ignored: nothing is ever stretched; widescreen is future work. */
static int n3ds_present_top(const uint32_t *argb, int w, int h, int pitch, int pin_43)
{
    (void)pin_43;
    if (!argb || w <= 0 || h <= 0 || pitch < w) return 0;
    static int use_cpu = -1;
    if (use_cpu < 0) {
        const char *e = getenv("THPS2_PRESENT");
        use_cpu = e && strcmp(e, "cpu") == 0;
        if (use_cpu) printf("thps2-n3ds: THPS2_PRESENT=cpu: CPU presenter\n");
    }
    if (snapshot_maybe(argb, w, h, pitch)) s_screen_snap_frame = (uint32_t)s_frame_count;
    int ok;
    if (!use_cpu && n3ds_gpu_present(argb, w, h, pitch)) {
        if (s_screen_snap_frame) {
            static uint32_t shot[400 * 240];
            if (n3ds_gpu_capture_top(shot)) {
                /* The capture is already y-down ARGB; store it rotated like
                 * the CPU path so both share one writer. */
                static uint32_t rot[400 * 240];
                for (int y = 0; y < 240; y++)
                    for (int x = 0; x < 400; x++)
                        rot[x * 240 + (239 - y)] = shot[y * 400 + x] << 8;
                screen_snapshot_write(rot, s_screen_snap_frame);
            }
            s_screen_snap_frame = 0;
        }
        ok = 1;
    } else {
        ok = present_top_cpu(argb, w, h, pitch);
    }
    if (ok) {
        g_n3ds_gfx_ready = 1;
        n3ds_hud_tick();      /* bottom-screen counter, redrawn once a second */
    }
    return ok;
}

/* ---- Scripted input and frame snapshots (opt-in testing aids) -------------
 *
 * input-script.txt in the data folder, one press per line:
 *     <guest frame> <frames held> <button>[+<button>...]   e.g.  1800 9 START
 * Buttons: 3DS names (A B X Y L R ZL ZR START SELECT UP DOWN LEFT RIGHT) or
 * the PS1 buttons SDL's 3DS gamepad mapping puts on them (CROSS=B, CIRCLE=A,
 * SQUARE=Y, TRIANGLE=X). Frames are the runtime's guest frame counter, so a
 * script lines up the same at any emulation speed (CD and timer timing are
 * guest-cycle based). The presses are ORed into libctru's hidKeysDown /
 * hidKeysUp / hidKeysHeld through link-time wrappers (CMakeLists.txt), which
 * is where SDL's 3DS joystick driver reads the buttons.
 *
 * THPS2_SNAPSHOTS=<frame>[,<frame>...] (psx-env.txt) saves the presented
 * frame as snap_<frame>.bmp in the data folder the first time each frame is
 * shown, so a scripted run can be checked without watching it. */

typedef struct { uint32_t frame, hold, keys; } ScriptPress;
static ScriptPress *s_script;
static unsigned s_script_n;
static u32 s_script_prev_keys, s_script_pending_up;

static const struct { const char *name; u32 key; } k_buttons[] = {
    {"A", KEY_A}, {"B", KEY_B}, {"X", KEY_X}, {"Y", KEY_Y}, {"L", KEY_L}, {"R", KEY_R},
    {"ZL", KEY_ZL}, {"ZR", KEY_ZR}, {"START", KEY_START}, {"SELECT", KEY_SELECT},
    {"UP", KEY_DUP}, {"DOWN", KEY_DDOWN}, {"LEFT", KEY_DLEFT}, {"RIGHT", KEY_DRIGHT},
    {"CROSS", KEY_B}, {"CIRCLE", KEY_A}, {"SQUARE", KEY_Y}, {"TRIANGLE", KEY_X},
};

static u32 script_parse_keys(char *spec)
{
    u32 keys = 0;
    for (char *tok = strtok(spec, "+"); tok; tok = strtok(NULL, "+")) {
        unsigned i;
        for (i = 0; i < sizeof k_buttons / sizeof k_buttons[0]; i++)
            if (!strcmp(tok, k_buttons[i].name)) { keys |= k_buttons[i].key; break; }
        if (i == sizeof k_buttons / sizeof k_buttons[0])
            printf("thps2-n3ds: input-script: unknown button '%s'\n", tok);
    }
    return keys;
}

static void script_load(void)
{
    FILE *f = fopen("input-script.txt", "r");
    if (!f) return;
    char line[128];
    unsigned cap = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long frame, hold;
        char spec[64];
        if (line[0] == '#' || sscanf(line, "%lu %lu %63s", &frame, &hold, spec) != 3) continue;
        if (s_script_n == cap) {
            cap = cap ? cap * 2 : 32;
            ScriptPress *grown = realloc(s_script, cap * sizeof *s_script);
            if (!grown) break;
            s_script = grown;
        }
        s_script[s_script_n++] = (ScriptPress){ (uint32_t)frame, (uint32_t)hold, script_parse_keys(spec) };
    }
    fclose(f);
    printf("thps2-n3ds: input-script: %u presses\n", s_script_n);
}

static u32 script_keys_now(void)
{
    const uint64_t f = s_frame_count;
    u32 keys = 0;
    for (unsigned i = 0; i < s_script_n; i++)
        if (f >= s_script[i].frame && f < (uint64_t)s_script[i].frame + s_script[i].hold)
            keys |= s_script[i].keys;
    return keys;
}

u32 __real_hidKeysDown(void);
u32 __real_hidKeysUp(void);
u32 __real_hidKeysHeld(void);

/* SDL calls hidKeysDown then hidKeysUp once per event pump. */
u32 __wrap_hidKeysDown(void)
{
    u32 real = __real_hidKeysDown();
    if (!s_script_n) return real;
    const u32 now = script_keys_now();
    const u32 down = now & ~s_script_prev_keys;
    s_script_pending_up |= s_script_prev_keys & ~now;
    s_script_prev_keys = now;
    return real | down;
}

u32 __wrap_hidKeysUp(void)
{
    u32 real = __real_hidKeysUp();
    const u32 up = s_script_pending_up;
    s_script_pending_up = 0;
    return real | up;
}

u32 __wrap_hidKeysHeld(void)
{
    return __real_hidKeysHeld() | (s_script_n ? s_script_prev_keys : 0u);
}

static uint32_t s_snap_frames[16];
static unsigned s_snap_n;

static void snapshots_load(void)
{
    const char *e = getenv("THPS2_SNAPSHOTS");
    if (!e) return;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", e);
    for (char *tok = strtok(buf, ","); tok && s_snap_n < 16; tok = strtok(NULL, ","))
        s_snap_frames[s_snap_n++] = (uint32_t)strtoul(tok, NULL, 10);
}

/* 24-bit bottom-up BMP of the ARGB frame. */
static void snapshot_write_named(const uint32_t *argb, int w, int h, int pitch, const char *base)
{
    char name[48];
    snprintf(name, sizeof name, "%s.bmp", base);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    const uint32_t row = ((uint32_t)w * 3u + 3u) & ~3u, size = 54u + row * (uint32_t)h;
    uint8_t hdr[54] = { 'B', 'M' };
    #define PUT32(o, v) do { hdr[o] = (uint8_t)(v); hdr[o+1] = (uint8_t)((v) >> 8); \
                             hdr[o+2] = (uint8_t)((v) >> 16); hdr[o+3] = (uint8_t)((v) >> 24); } while (0)
    PUT32(2, size); PUT32(10, 54u); PUT32(14, 40u); PUT32(18, (uint32_t)w); PUT32(22, (uint32_t)h);
    hdr[26] = 1; hdr[28] = 24; PUT32(34, row * (uint32_t)h);
    #undef PUT32
    fwrite(hdr, 1, sizeof hdr, f);
    uint8_t *line = calloc(1, row);
    for (int y = h - 1; line && y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            const uint32_t p = argb[(size_t)y * (size_t)pitch + (size_t)x];
            line[x * 3 + 0] = (uint8_t)p; line[x * 3 + 1] = (uint8_t)(p >> 8); line[x * 3 + 2] = (uint8_t)(p >> 16);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    printf("thps2-n3ds: snapshot %s (%dx%d)\n", name, w, h);
}

static void snapshot_write(const uint32_t *argb, int w, int h, int pitch, uint32_t frame)
{
    char base[32];
    snprintf(base, sizeof base, "snap_%lu", (unsigned long)frame);
    snapshot_write_named(argb, w, h, pitch, base);
}

static int snapshot_maybe(const uint32_t *argb, int w, int h, int pitch)
{
    const uint64_t f = s_frame_count;
    int taken = 0;
    for (unsigned i = 0; i < s_snap_n; i++) {
        if (s_snap_frames[i] && f >= s_snap_frames[i]) {
            if (!taken) snapshot_write(argb, w, h, pitch, (uint32_t)f);
            s_snap_frames[i] = 0;   /* once each */
            taken = 1;
        }
    }
    return taken;
}

/* screen_<frame>.bmp: the whole 400x240 top screen as the player sees it
 * (black bars included), from a rotated RGBA8 buffer laid out like the
 * panel's framebuffer: screen (x, y) at [x * 240 + (239 - y)]. */
static void screen_snapshot_write(const uint32_t *rotated_rgba, uint32_t frame)
{
    static uint32_t img[400 * 240];
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 400; x++)
            img[y * 400 + x] = rotated_rgba[x * 240 + (239 - y)] >> 8;
    char name[48];
    snprintf(name, sizeof name, "screen_%lu", (unsigned long)frame);
    snapshot_write_named(img, 400, 240, 400, name);
}

/* One line at process exit, so a run that ends without a message from the
 * runtime still shows that it ended normally rather than crashed. */
static void thps2_n3ds_note_exit(void)
{
    static const char bye[] = "thps2-n3ds: process exit";
    svcOutputDebugString(bye, sizeof(bye) - 1);
}

/* ---- Log file (opt-in) ----------------------------------------------------
 * An empty file named enable-log.txt in the data folder makes everything the
 * program prints (startup, [FPS] lines, errors) also go to log.txt there,
 * along with a status line each second. Writes are buffered and flushed once
 * a second and on a fatal error, so logging costs little. */
FILE *g_n3ds_log;

static ssize_t tee_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    (void)r; (void)fd;
    svcOutputDebugString(ptr, len);
    if (g_n3ds_log) fwrite(ptr, 1, len, g_n3ds_log);
    return (ssize_t)len;
}

static const devoptab_t k_tee = { .name = "tee", .structSize = 0, .write_r = tee_write };

void n3ds_log_flush(void)
{
    if (g_n3ds_log) fflush(g_n3ds_log);
}

__attribute__((constructor))
static void thps2_n3ds_process_setup(void)
{
    /* stdout and stderr -> svcOutputDebugString, and log.txt when enabled. */
    devoptab_list[STD_OUT] = &k_tee;
    devoptab_list[STD_ERR] = &k_tee;
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

    if (access("enable-log.txt", F_OK) == 0) {
        g_n3ds_log = fopen("log.txt", "w");
        if (g_n3ds_log) {
            static char logbuf[64 * 1024];
            setvbuf(g_n3ds_log, logbuf, _IOFBF, sizeof logbuf);
            bool n3ds = false;
            APT_CheckNew3DS(&n3ds);
            printf("thps2-n3ds: log started; build %s (psxrecomp %s); %s; heap %lu MB, linear %lu MB\n",
                   THPS2_N3DS_BUILD, THPS2_N3DS_PSXRECOMP, n3ds ? "New 3DS" : "Old 3DS",
                   (unsigned long)(envGetHeapSize() >> 20), (unsigned long)(envGetLinearHeapSize() >> 20));
            /* The runtime's once-a-second [FPS] line is the most useful entry. */
            setenv("PSX_FPS_TELEMETRY", "1", 0);
        }
    }

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

    script_load();      /* input-script.txt, if present */
    snapshots_load();   /* THPS2_SNAPSHOTS, if set */

    /* Opt-in: an empty file named wait-for-debugger in the data folder holds
     * the process here for 15 s so a debugger (Azahar's GDB stub, or GDB over
     * Luma3DS on hardware) can attach before anything else runs. */
    if (access("wait-for-debugger", F_OK) == 0) {
        static const char wait[] = "thps2-n3ds: waiting 15 s for a debugger";
        svcOutputDebugString(wait, sizeof(wait) - 1);
        svcSleepThread(15LL * 1000 * 1000 * 1000);
    }
}
