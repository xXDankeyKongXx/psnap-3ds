#pragma once
// Bottom screen (bottom_ui.c): drawn between C2D_SceneBegin(bottom) and C3D_FrameEnd.
#include <3ds.h>

#include "snap_game.h"

void bottom_ui_init(void);
void bottom_ui_exit(void);
void bottom_ui_input(u32 keys_down, const touchPosition* touch);
// Keys a tapped bottom-screen button holds (items in a course), added to the 3DS keys held.
u32 bottom_ui_keys(void);
void bottom_ui_draw(const char* rom_state, const char* boot_state, const SnapStats* stats);
// Before the game starts (rom_check.c): reading the ROM through the first time, and what to do
// when there's no usable ROM.
void bottom_ui_draw_rom_progress(const char* message, int percent);
void bottom_ui_draw_rom_help(const char* state, const char* found);
// ... and the top screen meanwhile.
void bottom_ui_draw_rom_top(const char* state);
