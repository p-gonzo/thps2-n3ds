/* n3ds_gpu_shadow.h — GPU renderer Phase 2 shadow mode (n3ds_gpu_shadow.c). */
#ifndef N3DS_GPU_SHADOW_H
#define N3DS_GPU_SHADOW_H

#include <stddef.h>
#include <stdint.h>

int  n3ds_shadow_enabled(void);   /* THPS2_GPU_SHADOW=1 or THPS2_GPU=1 */
int  n3ds_shadow_mode(void);      /* 0 off, 1 shadow beside the software renderer, 2 GPU presents */
/* psxrecomp host_present_vram hook: shows the displayed buffer from the GPU
 * copy when the GPU owns it (THPS2_GPU=1). 1 = shown. */
int  n3ds_shadow_present_vram(int x, int y, int w, int h, int depth24);
void n3ds_shadow_install(void);   /* registers the tap; call before the game runs */
void n3ds_shadow_render(void);    /* inside a citro3d frame: draw this frame's batches */
/* Copies GPU VRAM [x, x+w) x [y, y+h) as ARGB (y down). 0 if unavailable. */
int  n3ds_shadow_capture(int x, int y, int w, int h, uint32_t *out);
/* Counters since the last call, as text for the log; resets them. */
void n3ds_shadow_status(char *buf, size_t cap);

#endif
