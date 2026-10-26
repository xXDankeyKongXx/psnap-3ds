#ifndef SNAP_HOOKS_H
#define SNAP_HOOKS_H

// Functions called from the hooks in recomp/pokemonsnap.us.3ds.toml, plus libultra functions
// N64ModernRuntime doesn't implement. Included by every recompiled file via `recomp_include`.

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

// Hooks (source/port/snap_game.cpp)
void snap_on_overlay_load(uint8_t* rdram, recomp_context* ctx);
void snap_on_draw_sprite(uint8_t* rdram, recomp_context* ctx);
void snap_on_item(int kind);
void photo_thumb_begin(uint8_t* rdram, recomp_context* ctx); // photo_cache.cpp
void photo_thumb_end(uint8_t* rdram, recomp_context* ctx);
void photo_thumb_render(uint8_t* rdram, recomp_context* ctx);
uint32_t snap_ai_length(void);
void snap_cc_menu(uint8_t* rdram, recomp_context* ctx);    // camera_check.cpp
void snap_cc_confirm(uint8_t* rdram, recomp_context* ctx);

// libultra functions N64ModernRuntime doesn't implement (snap_game.cpp)
void __osSetWatchLo_recomp(uint8_t* rdram, recomp_context* ctx);
void __osContRamRead_recomp(uint8_t* rdram, recomp_context* ctx);
void __osContRamWrite_recomp(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
}
#endif

#endif
