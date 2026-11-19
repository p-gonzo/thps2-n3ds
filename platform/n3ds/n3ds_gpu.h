/* n3ds_gpu.h — the citro3d (PICA200) side of the 3DS port.
 *
 * Phase 1 of docs/N3DS_GPU_RENDERER.md: the PS1 frame the runtime composed on
 * the CPU is uploaded as a texture and drawn onto the top screen by the GPU,
 * at the PS1's 4:3 shape (320x240, centred, black bars), never stretched.
 * Later phases draw the game's primitives into this same target. */
#ifndef N3DS_GPU_H
#define N3DS_GPU_H

#include <stdint.h>

/* Draws one ARGB8888 frame (w x h, pitch in pixels) on the top screen and
 * presents it at the next vblank. Returns 0 if the GPU path is unavailable
 * (citro3d failed to start); the caller then presents on the CPU. */
int n3ds_gpu_present(const uint32_t *argb, int w, int h, int pitch);

/* Copies the last frame drawn on the top screen into out (400 x 240 ARGB,
 * row-major, y down): what the player sees, black bars included. Returns 0
 * if nothing has been drawn yet. */
int n3ds_gpu_capture_top(uint32_t *out);

#endif
