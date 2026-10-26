#ifndef C3D_BACKEND_H
#define C3D_BACKEND_H

// Main-thread side of the N64 renderer: draws the latest finished N64 frame on the top screen
// with Citro3D. C interface so main.c can call it.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// After C3D_Init: creates the top-screen render target, shader and buffers.
void n64_top_init(void);

// Between C3D_FrameBegin and C3D_FrameEnd. When frozen, keeps showing the last frame
// (used to pause the top screen while the bottom screen stays live).
bool n64_top_render(bool frozen); // true if it drew a new frame
bool n64_top_red_dot(void);
// Stereoscopic 3D: eye separation (0 = off); the right eye's target while on (else NULL).
void n64_top_set_stereo(float separation);
C3D_RenderTarget* n64_top_target_right(void);
void n64_top_redraw(void);
C3D_RenderTarget* n64_top_target(void);

void n64_top_exit(void);

// Main loop, before C3D_FrameBegin: finishes render-to-RAM for the off-screen image drawn last frame.
void n64_offscreen_before_frame(void);

// Draw calls and vertices in the frame currently shown (for the status line).
void n64_top_stats(unsigned* draws, unsigned* vertices);

// Development: log every draw of the frame currently shown.
void n64_top_dump_current(void);

// Draws the HUD gauge moved off the top screen (1 life, 2 weapon energy) into the current target
// (the bottom screen, inside a citro2d scene): top-screen pixels from (src_x, src_y) are placed at
// (dst_x, dst_y), scaled. False if the current frame has no such gauge.
bool n64_bottom_overlay(int region, float src_x, float src_y, float dst_x, float dst_y, float scale);

// Development: write the textures of the frame currently shown to a folder (see c3d_backend.cpp).
void n64_top_dump_textures(const char* dir);

#ifdef __cplusplus
}
#endif

#endif
