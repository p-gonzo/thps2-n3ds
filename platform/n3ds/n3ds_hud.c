/* n3ds_hud.c - bottom-screen status for the THPS2 3DS port.
 *
 * Once a second, after a top-screen present, the bottom screen shows:
 *   GAME  9.3 FPS   0.15X       guest frames per second, and that over 60
 *   FRAME 7123                  guest frame counter
 *   HEAP  40/91 MB              heap the game has allocated / heap size
 * The game never draws to the bottom screen, so this costs one redraw a
 * second. Drawn with a built-in 5x7 font straight into the RGBA8 bottom
 * framebuffer SDL set up (libctru's console would switch the screen to RGB565
 * and take over stdout, which goes to the debug log here).
 *
 * n3ds_hud_message() draws a full-screen message on the bottom screen; the
 * fatal-error handler (n3ds_fatal.cpp) uses it.
 */
#include <3ds.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include "n3ds_hud.h"

extern uint64_t s_frame_count;   /* psxrecomp: guest frames since boot */
extern FILE *g_n3ds_log;         /* n3ds_platform.c; NULL unless enable-log.txt */
void n3ds_log_flush(void);

/* Ticks (268 MHz system timer, independent of the CPU clock) for a fixed
 * loop of dependent instructions. */
static u64 spin_ticks(void)
{
    u32 n = 4u * 1000u * 1000u;
    const u64 t0 = svcGetSystemTick();
    __asm__ volatile("1: subs %0, %0, #1\n\tbne 1b" : "+r"(n) :: "cc");
    return svcGetSystemTick() - t0;
}

/* Is the New 3DS high-speed mode (804 MHz + L2) actually in effect? Time
 * the same loop with the mode off and on; the ratio is ~3 when it works,
 * ~1 when the request has no effect. Measured once, at the first tick. */
static double s_cpu_ratio;
static void measure_cpu_mode(void)
{
    const u64 fast1 = spin_ticks();
    osSetSpeedupEnable(false);
    const u64 slow = spin_ticks();
    osSetSpeedupEnable(true);
    const u64 fast2 = spin_ticks();
    /* fast1 is the mode the game was actually running in. */
    s_cpu_ratio = fast1 ? (double)slow / (double)fast1 : 0.0;
    printf("thps2-n3ds: cpu check: loop %llu ticks in normal mode, %llu with speedup (first %llu); "
           "ratio (normal / as running) %.2f -> %s (emulators: ignore)\n", (unsigned long long)slow, (unsigned long long)fast2,
           (unsigned long long)fast1, s_cpu_ratio,
           s_cpu_ratio > 2.5 ? "804 MHz mode active" : "speedup NOT in effect (268 MHz)");
}

int g_n3ds_gfx_ready = 0;

/* libctru takes all free application memory as the heap at start, so the
 * system's "used" figure is always everything; what matters is how much of
 * that heap malloc has handed out. */
size_t n3ds_heap_used(void)
{
    struct mallinfo mi = mallinfo();
    return (size_t)mi.uordblks + (size_t)mi.hblkhd;
}        /* set once the top screen has been presented */

/* 5x7 glyphs, one byte per row, bit 4 = leftmost column. */
static const struct { char c; uint8_t rows[7]; } k_font[] = {
    {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, {'3',{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'D',{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}}, {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}}, {'J',{0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'N',{0x11,0x11,0x19,0x15,0x13,0x11,0x11}},
    {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}}, {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W',{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}}, {'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y',{0x11,0x11,0x11,0x0A,0x04,0x04,0x04}}, {'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    {'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}}, {':',{0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}},
    {'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}}, {'/',{0x01,0x01,0x02,0x04,0x08,0x10,0x10}},
    {'%',{0x18,0x19,0x02,0x04,0x08,0x13,0x03}}, {'(',{0x02,0x04,0x08,0x08,0x08,0x04,0x02}},
    {')',{0x08,0x04,0x02,0x02,0x02,0x04,0x08}}, {'\'',{0x04,0x04,0x08,0x00,0x00,0x00,0x00}},
};

static const uint8_t *glyph(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (unsigned i = 0; i < sizeof k_font / sizeof k_font[0]; i++)
        if (k_font[i].c == c) return k_font[i].rows;
    return NULL;   /* space and anything unknown: blank */
}

/* Bottom framebuffer, 320x240, stored rotated: (x, y) at fb[x * 240 + 239 - y]. */
static u32 *bottom_fb(void)
{
    if (gfxGetScreenFormat(GFX_BOTTOM) != GSP_RGBA8_OES) return NULL;
    u16 rows = 0, cols = 0;
    u32 *fb = (u32 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &rows, &cols);
    return (fb && rows == 240 && cols == 320) ? fb : NULL;
}

static void draw_text(u32 *fb, int x, int y, int scale, u32 rgba, const char *s)
{
    for (; *s; s++, x += 6 * scale) {
        const uint8_t *g = glyph(*s);
        if (!g) continue;
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] & (0x10 >> c))
                    for (int dy = 0; dy < scale; dy++)
                        for (int dx = 0; dx < scale; dx++) {
                            const int px = x + c * scale + dx, py = y + r * scale + dy;
                            if (px >= 0 && px < 320 && py >= 0 && py < 240)
                                fb[px * 240 + (239 - py)] = rgba;
                        }
    }
}

static void finish(u32 *fb)
{
    GSPGPU_FlushDataCache(fb, 320u * 240u * 4u);
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

#define WHITE  0xFFFFFFFFu
#define GREY   0xA0A0A0FFu
#define YELLOW 0xFFD040FFu
#define BG     0x101820FFu

void n3ds_hud_tick(void)
{
    static u64 t0;
    static uint64_t f0;
    const u64 now = svcGetSystemTick();
    if (!t0) { measure_cpu_mode(); t0 = svcGetSystemTick(); f0 = s_frame_count; return; }
    const double secs = (double)(now - t0) / (double)SYSCLOCK_ARM11;
    if (secs < 1.0) return;
    const double fps = (double)(s_frame_count - f0) / secs;
    t0 = now;
    f0 = s_frame_count;

    u32 *fb = bottom_fb();
    if (!fb) return;
    for (int i = 0; i < 320 * 240; i++) fb[i] = BG;
    char line[48];
    draw_text(fb, 12, 12, 2, GREY, "THPS2 NEW 3DS TEST BUILD");   /* 25 chars fit at scale 2 */
    snprintf(line, sizeof line, "%.1f FPS", fps);
    draw_text(fb, 12, 50, 5, YELLOW, line);
    snprintf(line, sizeof line, "%.2fX PS1 SPEED", fps / 59.94);
    draw_text(fb, 12, 106, 3, WHITE, line);
    snprintf(line, sizeof line, "FRAME %lu", (unsigned long)s_frame_count);
    draw_text(fb, 12, 146, 3, WHITE, line);
    snprintf(line, sizeof line, "HEAP %lu/%lu MB", (unsigned long)(n3ds_heap_used() >> 20),
             (unsigned long)(envGetHeapSize() >> 20));
    draw_text(fb, 12, 180, 3, WHITE, line);
    snprintf(line, sizeof line, "CPU %s X%.1f%s", s_cpu_ratio > 2.5 ? "804MHZ" : "268MHZ",
             s_cpu_ratio, g_n3ds_log ? "  LOG ON" : "");
    draw_text(fb, 12, 214, 2, s_cpu_ratio > 2.5 ? GREY : YELLOW, line);
    if (g_n3ds_log) {
        printf("thps2-n3ds: status fps=%.2f frame=%lu heap=%lu/%lu MB\n", fps,
               (unsigned long)s_frame_count, (unsigned long)(n3ds_heap_used() >> 20),
               (unsigned long)(envGetHeapSize() >> 20));
        n3ds_log_flush();
    }
    finish(fb);
}

void n3ds_hud_message(const char *title, const char *detail, const char *footer)
{
    u32 *fb = bottom_fb();
    if (!fb) return;
    for (int i = 0; i < 320 * 240; i++) fb[i] = 0x401010FFu;
    draw_text(fb, 12, 16, 3, YELLOW, title);
    /* Wrap the detail at 25 characters (scale 2 = 12 px per character). */
    int y = 60;
    char buf[26];
    for (const char *p = detail ? detail : ""; *p && y < 200; y += 18) {
        size_t n = strlen(p) > 25 ? 25 : strlen(p);
        if (n == 25) {   /* break at the last space if there is one */
            size_t k = n;
            while (k > 0 && p[k] != ' ') k--;
            if (k > 0) n = k;
        }
        memcpy(buf, p, n);
        buf[n] = '\0';
        draw_text(fb, 12, y, 2, WHITE, buf);
        p += n;
        while (*p == ' ') p++;
    }
    draw_text(fb, 12, 218, 2, GREY, footer ? footer : "");
    finish(fb);
}
