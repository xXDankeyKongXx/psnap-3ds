#pragma once
// Finding and checking the player's Pokemon Snap ROM (rom_check.c).
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ok;
    char path[160];   // the ROM to boot (ok)
    char state[80];   // one line for the log and the bottom screen
    char found[96];   // not ok: what was found instead, if anything
} RomCheck;

// SHA-1 of Pokemon Snap (USA), in .z64 byte order.
#define ROM_SHA1_USA "edc7c49cc568c045fe48be0d18011c30f393cbaf"

// Called while a ROM is read through (a few seconds, the first time): what's happening, 0-100.
typedef void (*RomProgress)(const char* message, int percent);

// The ROM packed into the app if there is one; else data_path/pokemonsnap.us.z64, checked once (its
// SHA-1, remembered in data_path/rom_verified.txt); else any 16 MB file in data_path or
// sdmc:/roms/n64/ in .z64, .v64 or .n64 byte order that is the USA game, put into
// data_path/pokemonsnap.us.z64 as .z64.
void rom_check(RomCheck* out, const char* data_path, const char* romfs_path, RomProgress progress);

#ifdef __cplusplus
}
#endif
