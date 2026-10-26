#pragma once
// Camera Check on the touch screen (camera_check.cpp): the photos as a grid on the bottom screen,
// the chosen one large on the top screen, Oak's Mark / Album Mark / To Prof. Oak as touch buttons.
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CC_MAX_PHOTOS 60

enum {
    CC_NEW = 1,        // a Pokemon not in the Report yet (the grid's NEW ribbon)
    CC_OAK = 2,        // Oak's Mark
    CC_ALBUM = 4,      // Album Mark
    CC_OAK_OK = 8,     // can have Oak's Mark (a Pokemon, or a PKMN Sign once there's the dash engine)
    CC_NO_POKEMON = 16,
};

typedef struct {
    uint64_t hash;     // photo_cache key
    int16_t species;   // 1-151, a Sign id, 9999 none
    uint8_t marks;     // CC_*
} CcPhoto;

typedef enum {
    CC_OFF,            // not in Camera Check's menu: the game's own screens
    CC_MENU,           // the touch screen version is in charge
    CC_CONFIRM,        // the game asks "Show this picture to Prof. Oak?" (its own top screen)
} CcMode;

typedef struct {
    int count;
    CcPhoto photos[CC_MAX_PHOTOS];
    bool album_open;   // Album Marks allowed (after the tutorial, with room in the album)
    int album_left;    // pictures that can still be marked for the album
    bool marked;       // any photo marked at all
} CcView;

CcMode cc_view(CcView* out);       // out may be NULL
int cc_selected(void);
void cc_select(int index);         // shown on the top screen
void cc_toggle_oak(int index);
void cc_toggle_album(int index);
void cc_send(void);                // "To Prof. Oak": the game then asks
void cc_answer(bool yes);          // ... and this answers it

// The top screen (n64_renderer.cpp): the photo to show large and its marks, while CC_MENU.
bool cc_top(uint64_t* hash, uint8_t* marks);

#ifdef __cplusplus
}
#endif
