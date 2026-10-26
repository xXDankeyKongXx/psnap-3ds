#pragma once
// Photo thumbnails captured from the game (photo_cache.cpp).
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void photo_cache_init(const char* data_path);
uint64_t photo_cache_hash(const uint8_t* rdram, uint32_t photo_address);
// Watchdog thread: writes new thumbnails to DATA_PATH/photos/.
void photo_cache_flush(void);
// A thumbnail by PhotoData hash (memory, else the SD card): RGBA5551 rows, top to bottom.
bool photo_cache_get(uint64_t hash, int* w, int* h, uint16_t* pixels, int max_pixels);
// Its width (0: none in memory or on the SD card; 280 for a big view's render).
int photo_cache_width(uint64_t hash);
// Whether RDRAM (physical) address lies in a bitmap a photo was printed into (stereoscopic 3D).
bool photo_is_bitmap(uint32_t phys);
// Forgets those bitmaps (a new scene: their memory goes to other things).
void photo_bitmaps_clear(void);

#ifdef __cplusplus
}
#endif
