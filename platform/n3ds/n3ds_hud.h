/* n3ds_hud.h - bottom-screen status and messages (n3ds_hud.c). */
#ifndef THPS2_N3DS_HUD_H
#define THPS2_N3DS_HUD_H

#ifdef __cplusplus
extern "C" {
#endif

/* 1 once the top screen has been presented (gfx is initialised). */
extern int g_n3ds_gfx_ready;

/* Bytes of heap the program has allocated. */
#include <stddef.h>
size_t n3ds_heap_used(void);

/* Call after each top-screen present; redraws the counter once a second. */
void n3ds_hud_tick(void);

/* Full bottom-screen message (title, wrapped detail, footer line). */
void n3ds_hud_message(const char *title, const char *detail, const char *footer);

#ifdef __cplusplus
}
#endif

#endif
