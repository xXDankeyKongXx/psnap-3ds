// Bottom screen, by scene (snap_scene):
// - Oak's Lab: over a dimmed picture of the lab, settings (gyro aiming, widescreen: on/off).
// - In a course: over a faded picture of Pokemon Island, the course's best photo (top left, from
//   the save's Pokemon Report: photo, Pokemon, score; a blank photo with "No Score" before there is
//   one), the course name on a roll of film (the film counter's art, top right) and the items as
//   touch buttons (apple, Pester Ball, Poke Flute, dash), which press the N64 button the game uses
//   for them; greyed out until the game has unlocked them. The top screen's item icons are hidden
//   (snap_on_draw_sprite).
// - Camera Check (camera_check.cpp): the photos as a grid of the game's renders, pages turned with
//   its orange knobs, its Oak's Mark / Album Mark / To Prof. Oak buttons (tools/make_cc_parts.py);
//   tapping a photo shows it large on the top screen. Then the game's own "Show this picture to
//   Prof. Oak?" with its two answers as buttons.
// Text is drawn in the game's own 12x12 font (gfx/font.png, tools/make_font.py).
// - Elsewhere: the status page.

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>

#include "bottom_ui.h"
#include "camera_check.h"
#include "game_font.h"
#include "gyro.h"
#include "photo_cache.h"
#include "rom_check.h"
#include "settings.h"
#include "snap_game.h"
#include "ui_t3x.h"

static C2D_SpriteSheet sheet;

enum { // gfx/ui.t3s order
    SHEET_ISLAND, SHEET_APPLE, SHEET_PESTER, SHEET_FLUTE, SHEET_DASH,
    SHEET_APPLE_OFF, SHEET_PESTER_OFF, SHEET_FLUTE_OFF, SHEET_DASH_OFF,
    SHEET_FONT, SHEET_FILM_HEAD, SHEET_FILM_STRIP, SHEET_LAB,
    SHEET_BUTTON, SHEET_FOCUS_TL, SHEET_FOCUS_TR, SHEET_FOCUS_BL, SHEET_FOCUS_BR,
    SHEET_CC_TILE, SHEET_CC_OAK, SHEET_CC_ALBUM, SHEET_CC_SEND, SHEET_CC_MARK_OAK, SHEET_CC_MARK_ALBUM,
    SHEET_CC_NEW, SHEET_CC_KNOB_L, SHEET_CC_KNOB_R,
};

// Part of a sheet image (pixels), for the font's glyph cells and the film's pieces.
static C2D_Image sub_image(int index, int x, int y, int w, int h, Tex3DS_SubTexture* sub) {
    C2D_Image img = C2D_SpriteSheetGetImage(sheet, index);
    const Tex3DS_SubTexture* s = img.subtex;
    float du = (s->right - s->left) / s->width, dv = (s->bottom - s->top) / s->height;
    *sub = (Tex3DS_SubTexture){ (u16)w, (u16)h, s->left + x * du, s->top + y * dv, s->left + (x + w) * du, s->top + (y + h) * dv };
    img.subtex = sub;
    return img;
}

// The game's font: glyphs 12x12, proportionally spaced (1 pixel apart), a 1-pixel black shadow like
// the game's text. size >= 0.7 draws at twice the size.
static int text_width(const char* s, int scale) {
    int w = 0;
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        w += (c >= 32 && c < 128 ? game_font_width[c - 32] : 4) + 1;
    }
    return (w > 0 ? w - 1 : 0) * scale;
}

static void glyphs(float x, float y, int scale, u32 color, const char* s) {
    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, color, 1.0f);
    for (; *s; s++) {
        unsigned c = (unsigned char)*s;
        if (c < 32 || c >= 128) c = '?';
        int w = game_font_width[c - 32];
        if (c != ' ' && w > 0) {
            Tex3DS_SubTexture sub;
            C2D_Image g = sub_image(SHEET_FONT, ((c - 32) % 16) * 12, ((c - 32) / 16) * 12, 12, 12, &sub);
            C2D_DrawImageAt(g, x, y, 0.6f, &tint, scale, scale);
        }
        x += (w + 1) * scale;
    }
}

static void text(float x, float y, float size, u32 color, const char* s, u32 flags) {
    int scale = size >= 0.7f ? 2 : 1;
    int w = text_width(s, scale);
    if (flags & C2D_AlignCenter) x -= w / 2;
    else if (flags & C2D_AlignRight) x -= w;
    x = (int)x;
    y = (int)y;
    glyphs(x + scale, y + scale, scale, C2D_Color32(0, 0, 0, (color >> 24) & 0xFF), s);
    glyphs(x, y, scale, color, s);
}

static bool inside(const touchPosition* t, int x, int y, int w, int h) {
    return t->px >= x && t->px < x + w && t->py >= y && t->py < y + h;
}

// --- Game data -------------------------------------------------------------------------------------

u8* snap_debug_rdram(void);

static u32 ram32(u32 a) {
    u8* rdram = snap_debug_rdram();
    return rdram ? *(u32*)(rdram + (a & 0x00FFFFFC)) : 0;
}
static u8 ram8(u32 a) {
    u8* rdram = snap_debug_rdram();
    return rdram ? rdram[(a & 0x00FFFFFF) ^ 3] : 0;
}

// Save data (more_funcs/5BF20.c): pointer at 0x800C21B0. +0x64 flags (apple bit 15, Pester Ball 14,
// flute 13, Zero-One dash 10), +0x6C the Pokemon Report's score per slot (69), +0x180 its photo per
// slot (PhotoData, 0x3A0 each; unused when the word at +4 is -1; course = top 7 bits of byte 0).
// Species -> slot: s8 table at 0x800AE4E4 (index species - 1).
#define SAVE_PTR 0x800C21B0u
#define REPORT_SLOTS 69

static const char* const course_names[7] = { "Beach", "Tunnel", "Volcano", "River", "Cave", "Valley", "Rainbow Cloud" };

static const char* const pokemon_names[152] = { "",
    "Bulbasaur", "Ivysaur", "Venusaur", "Charmander", "Charmeleon", "Charizard", "Squirtle", "Wartortle", "Blastoise",
    "Caterpie", "Metapod", "Butterfree", "Weedle", "Kakuna", "Beedrill", "Pidgey", "Pidgeotto", "Pidgeot", "Rattata",
    "Raticate", "Spearow", "Fearow", "Ekans", "Arbok", "Pikachu", "Raichu", "Sandshrew", "Sandslash", "Nidoran F",
    "Nidorina", "Nidoqueen", "Nidoran M", "Nidorino", "Nidoking", "Clefairy", "Clefable", "Vulpix", "Ninetales",
    "Jigglypuff", "Wigglytuff", "Zubat", "Golbat", "Oddish", "Gloom", "Vileplume", "Paras", "Parasect", "Venonat",
    "Venomoth", "Diglett", "Dugtrio", "Meowth", "Persian", "Psyduck", "Golduck", "Mankey", "Primeape", "Growlithe",
    "Arcanine", "Poliwag", "Poliwhirl", "Poliwrath", "Abra", "Kadabra", "Alakazam", "Machop", "Machoke", "Machamp",
    "Bellsprout", "Weepinbell", "Victreebel", "Tentacool", "Tentacruel", "Geodude", "Graveler", "Golem", "Ponyta",
    "Rapidash", "Slowpoke", "Slowbro", "Magnemite", "Magneton", "Farfetch'd", "Doduo", "Dodrio", "Seel", "Dewgong",
    "Grimer", "Muk", "Shellder", "Cloyster", "Gastly", "Haunter", "Gengar", "Onix", "Drowzee", "Hypno", "Krabby",
    "Kingler", "Voltorb", "Electrode", "Exeggcute", "Exeggutor", "Cubone", "Marowak", "Hitmonlee", "Hitmonchan",
    "Lickitung", "Koffing", "Weezing", "Rhyhorn", "Rhydon", "Chansey", "Tangela", "Kangaskhan", "Horsea", "Seadra",
    "Goldeen", "Seaking", "Staryu", "Starmie", "Mr. Mime", "Scyther", "Jynx", "Electabuzz", "Magmar", "Pinsir",
    "Tauros", "Magikarp", "Gyarados", "Lapras", "Ditto", "Eevee", "Vaporeon", "Jolteon", "Flareon", "Porygon",
    "Omanyte", "Omastar", "Kabuto", "Kabutops", "Aerodactyl", "Snorlax", "Articuno", "Zapdos", "Moltres", "Dratini",
    "Dragonair", "Dragonite", "Mewtwo", "Mew" };

// --- Best photo of the course ------------------------------------------------------------------------

static struct {
    int course;          // course it was looked up for (-1: none)
    int score;           // 0: no report photo for this course yet
    int species;
    u64 hash;            // the photo's PhotoData hash (photo_cache)
    bool have_image;
    int w, h;
    int retry_frames;    // looking the thumbnail up again (it may be captured later)
    C3D_Tex tex;
    bool tex_ready;
    Tex3DS_SubTexture sub;
} best = { .course = -1 };

static u16 thumb_px[320 * 240];

static u32 morton(u32 x, u32 y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

// RGBA5551 rows (top first) into a 512x256 RGBA5551 texture: 8x8 Morton tiles. The GPU's v = 1 is
// the first row in memory, so the image goes in top row first and its subtexture runs from top 1.0
// down (top above bottom: Tex3DS reads top < bottom as a rotated image).
static void upload_thumb(int w, int h) {
    if (!best.tex_ready) {
        if (!C3D_TexInit(&best.tex, 512, 256, GPU_RGBA5551)) return;
        C3D_TexSetFilter(&best.tex, GPU_LINEAR, GPU_LINEAR);
        best.tex_ready = true;
    }
    u16* dst = (u16*)best.tex.data;
    memset(dst, 0, 512 * 256 * 2);
    for (int y = 0; y < h; y++) {
        u32 r = (u32)y;
        for (int x = 0; x < w; x++) {
            u32 off = ((r / 8) * (512 / 8) + (u32)x / 8) * 64 + morton((u32)x % 8, r % 8);
            dst[off] = thumb_px[y * w + x] | 1; // opaque
        }
    }
    C3D_TexFlush(&best.tex);
    best.sub = (Tex3DS_SubTexture){ (u16)w, (u16)h, 0.0f, 1.0f, w / 512.0f, 1.0f - h / 256.0f };
}

static void find_best(int course) {
    best.course = course;
    best.score = 0;
    best.have_image = false;
    best.retry_frames = 0;
    // Development: DATA_PATH/besttest.txt names a thumbnail (its hash) to show in every course.
    if (FILE* f = fopen("sdmc:/3ds/psnap/besttest.txt", "r")) {
        unsigned long long h = 0;
        if (fscanf(f, "%llx", &h) == 1) {
            best.hash = h;
            best.score = 1234;
            best.species = 16;
        }
        fclose(f);
        if (best.score) return;
    }
    u32 save = ram32(SAVE_PTR);
    if (save < 0x80000000u || save >= 0x80800000u) return;
    int best_slot = -1;
    for (int slot = 0; slot < REPORT_SLOTS; slot++) {
        u32 photo = save + 0x180 + (u32)slot * 0x3A0;
        if (ram32(photo + 4) == 0xFFFFFFFFu) continue;
        int level = (s8)ram8(photo) >> 1;
        int score = (int)ram32(save + 0x6C + (u32)slot * 4);
        if (level != course || score <= best.score) continue;
        best.score = score;
        best_slot = slot;
    }
    if (best_slot < 0) return;
    best.species = 0;
    for (int id = 1; id <= 151; id++) {
        if ((s8)ram8(0x800AE4E4u + (u32)id - 1) == best_slot) {
            best.species = id;
            break;
        }
    }
    u8* rdram = snap_debug_rdram();
    best.hash = photo_cache_hash(rdram, save + 0x180 + (u32)best_slot * 0x3A0);
}

static void update_best(void) {
    int course = snap_course();
    if (course != best.course) find_best(course);
    if (best.score > 0 && !best.have_image && best.retry_frames-- <= 0) {
        best.retry_frames = 300;
        if (photo_cache_get(best.hash, &best.w, &best.h, thumb_px, 320 * 240)) {
            upload_thumb(best.w, best.h);
            best.have_image = best.tex_ready;
        }
    }
}

// --- Course page ---------------------------------------------------------------------------------------

typedef struct {
    int sprite;
    const char* label;
    u32 key;      // 3DS key that presses the N64 button for it (input_3ds.cpp)
    int sprite_off; // greyed out
    u32 sobj_ptr; // where the game keeps its HUD icon's sprite object (snap_game.cpp, snap_on_draw_sprite)
    int x, y;
} ItemButton;

#define ITEM_SIZE 60
static const ItemButton items[4] = {
    { SHEET_APPLE, "Apple", KEY_A, SHEET_APPLE_OFF, 0x80388224u, 14, 164 },
    { SHEET_PESTER, "Pester Ball", KEY_B, SHEET_PESTER_OFF, 0x80388228u, 90, 164 },
    { SHEET_FLUTE, "Poke Flute", KEY_Y, SHEET_FLUTE_OFF, 0x8038822Cu, 166, 164 },
    { SHEET_DASH, "Dash", KEY_R, SHEET_DASH_OFF, 0x80388194u, 242, 164 },
};

static u32 tap_keys;    // keys a tapped button holds
static int tap_frames;  // for this many more UI frames
static int tapped_item = -1;

// The game still animates its (hidden) HUD icons: shrink and restore when an item is thrown, a pulse
// while the flute plays. Their sprite scale (Sprite.scalex/scaley, sprite object +0x18/+0x1C) is
// applied to the buttons, so they throb the same way.
static void icon_scale(const ItemButton* b, float* sx, float* sy) {
    *sx = *sy = 1.0f;
    u32 sobj = ram32(b->sobj_ptr);
    if (sobj < 0x80000000u || sobj >= 0x80800000u) return;
    u32 wx = ram32(sobj + 0x18), wy = ram32(sobj + 0x1C);
    float fx, fy;
    memcpy(&fx, &wx, 4);
    memcpy(&fy, &wy, 4);
    if (fx > 0.2f && fx < 2.0f) *sx = fx;
    if (fy > 0.2f && fy < 2.0f) *sy = fy;
}

// What the game itself has unlocked for this course run (app_level/icons.c, set up by Icons_Init
// from the save's progress flags): Icons_MainObject (0x80388230) once set up, Icons_NumItemsAvailable
// (0x80388238: 1 apple, 2 + Pester Ball, 3 + flute), the dash icon's sprite object only with the
// Zero-One's dash engine.
static bool has_item(const ItemButton* b) {
    if (snap_scene() != SNAP_SCENE_COURSE || ram32(0x80388230u) == 0) return false;
    int index = (int)(b - items);
    if (index == 3) return ram32(b->sobj_ptr) >= 0x80000000u && ram32(b->sobj_ptr) < 0x80800000u;
    return (int)ram32(0x80388238u) >= index + 1;
}

// A title on a roll of film (top right): the film counter's canister (gfx/film_head.png, its first
// 12 columns) and a seamless strip made from its film (gfx/film_strip.png, tools/make_film.py).
static void film_title(const char* title) {
    const float fx = 148, fy = 10, end = 316;
    Tex3DS_SubTexture sub;
    C2D_DrawImageAt(sub_image(SHEET_FILM_HEAD, 0, 0, 12, 18, &sub), fx, fy, 0.3f, NULL, 2.0f, 2.0f);
    C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_FILM_STRIP), fx + 24, fy, 0.3f, NULL, 2.0f, 2.0f);
    text(fx + 24 + (end - fx - 24) / 2, fy + 12, 0.5f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF), title, C2D_AlignCenter);
}

#define PHOTO_X 8
#define PHOTO_Y 8
#define PHOTO_W 136
#define PHOTO_H 102

static void draw_course(void) {
    u32 white = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF), shadow = C2D_Color32(0x00, 0x00, 0x00, 0xA0);
    // Pokemon Island, faded.
    C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_ISLAND), 0, 0, 0.1f, NULL, 1.0f, 1.0f);
    C2D_DrawRectSolid(0, 0, 0.15f, 320, 240, C2D_Color32(0x08, 0x18, 0x30, 0x70));

    film_title(snap_course() >= 0 && snap_course() < 7 ? course_names[snap_course()] : "");

    // Best photo: white print frame, then the photo or a blank one.
    C2D_DrawRectSolid(PHOTO_X - 2, PHOTO_Y - 2, 0.2f, PHOTO_W + 4, PHOTO_H + 30, shadow);
    C2D_DrawRectSolid(PHOTO_X, PHOTO_Y, 0.25f, PHOTO_W, PHOTO_H + 26, white);
    int ix = PHOTO_X + 4, iy = PHOTO_Y + 4, iw = PHOTO_W - 8, ih = PHOTO_H - 8;
    if (best.score > 0 && best.have_image) {
        C2D_Image img = { &best.tex, &best.sub };
        C2D_DrawImageAt(img, ix, iy, 0.3f, NULL, (float)iw / best.w, (float)ih / best.h);
    } else {
        C2D_DrawRectSolid(ix, iy, 0.3f, iw, ih, C2D_Color32(0xC8, 0xCC, 0xD0, 0xFF));
        if (best.score <= 0) text(ix + iw / 2.0f, iy + ih / 2.0f - 9, 0.6f, C2D_Color32(0x60, 0x66, 0x70, 0xFF), "No Score", C2D_AlignCenter);
    }
    // Under the photo, on the print: the Pokemon's name in "Snap" red, its score in the logo's blue.
    if (best.score > 0) {
        const char* name = best.species > 0 && best.species <= 151 ? pokemon_names[best.species] : "?";
        char score[16];
        snprintf(score, sizeof(score), "%d", best.score);
        int gap = 8, w = text_width(name, 1) + gap + text_width(score, 1);
        float x = PHOTO_X + (PHOTO_W - w) / 2.0f, y = PHOTO_Y + PHOTO_H + 4;
        text(x, y, 0.5f, C2D_Color32(0xD0, 0x30, 0x10, 0xFF), name, 0);
        text(x + text_width(name, 1) + gap, y, 0.5f, C2D_Color32(0x1D, 0x3F, 0x8C, 0xFF), score, 0);
    } else {
        text(PHOTO_X + PHOTO_W / 2.0f, PHOTO_Y + PHOTO_H + 4, 0.5f, C2D_Color32(0x30, 0x30, 0x30, 0xFF), "Best photo", C2D_AlignCenter);
    }

    // Items.
    for (int i = 0; i < 4; i++) {
        const ItemButton* b = &items[i];
        bool have = has_item(b);
        bool pressed = tapped_item == i && tap_frames > 0;
        float x = b->x, y = b->y;
        // Round buttons (gfx/icon_*.png, 60x60 circles): a soft shadow, a yellow ring while pressed.
        float cx = x + ITEM_SIZE / 2.0f, cy = y + ITEM_SIZE / 2.0f;
        float sx, sy;
        icon_scale(b, &sx, &sy);
        float r = ITEM_SIZE / 2.0f * (sx < sy ? sx : sy);
        C2D_DrawCircleSolid(cx + 2, cy + 3, 0.2f, r, shadow);
        if (pressed) C2D_DrawCircleSolid(cx, cy, 0.22f, r + 4, C2D_Color32(0xFF, 0xE0, 0x60, 0xFF));
        C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, have ? b->sprite : b->sprite_off), cx - ITEM_SIZE * sx / 2.0f,
            cy - ITEM_SIZE * sy / 2.0f, 0.3f, NULL, sx, sy);
        text(x + ITEM_SIZE / 2.0f, y - 16, 0.42f, have ? white : C2D_Color32(0xB0, 0xB0, 0xB0, 0xFF), b->label, C2D_AlignCenter);
    }
}

// --- Camera Check ------------------------------------------------------------------------------------

#define CC_COLS 4
#define CC_PAGE 12
#define CC_TW 64
#define CC_TH 48
#define CC_GAP 5
#define CC_X0 25
#define CC_Y0 24
#define CC_KNOB_Y 66
#define CC_BTN_Y 212
#define CC_BTN_W 92
#define CC_BTN_H 21
static const int cc_btn_x[3] = { 10, 114, 218 };

static int cc_page;          // shown page (follows the chosen photo)
static int cc_flash = -1, cc_flash_frames; // a tapped button lights up
static char cc_note[96];     // what the last tap couldn't do, for a while
static int cc_note_frames;

// The page's photos, scaled down to the grid's size on the CPU (box filter) into one 512x128
// RGBA5551 texture, 8 x 2 slots of 64x48 (rows top first, as upload_thumb).
static C3D_Tex cc_tex;
static bool cc_tex_ready;
static u64 cc_slot_hash[CC_PAGE];
static int cc_slot_w[CC_PAGE];
static bool cc_slot_ok[CC_PAGE];

static void cc_fill_slot(int slot, int sw, int sh) {
    u16* dst = (u16*)cc_tex.data;
    int x0 = (slot % 8) * CC_TW, y0 = (slot / 8) * CC_TH;
    for (int y = 0; y < CC_TH; y++) {
        int sy0 = y * sh / CC_TH, sy1 = (y + 1) * sh / CC_TH;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < CC_TW; x++) {
            int sx0 = x * sw / CC_TW, sx1 = (x + 1) * sw / CC_TW;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            u32 r = 0, g = 0, b = 0, n = 0;
            for (int yy = sy0; yy < sy1; yy++) {
                for (int xx = sx0; xx < sx1; xx++) {
                    u16 p = thumb_px[yy * sw + xx];
                    r += (p >> 11) & 31;
                    g += (p >> 6) & 31;
                    b += (p >> 1) & 31;
                    n++;
                }
            }
            u16 px = (u16)(((r / n) << 11) | ((g / n) << 6) | ((b / n) << 1) | 1);
            u32 tx = (u32)(x0 + x), ty = (u32)(y0 + y);
            dst[((ty / 8) * (512 / 8) + tx / 8) * 64 + morton(tx % 8, ty % 8)] = px;
        }
    }
}

static void cc_update_thumbs(const CcView* v) {
    if (!cc_tex_ready) {
        if (!C3D_TexInit(&cc_tex, 512, 128, GPU_RGBA5551)) return;
        C3D_TexSetFilter(&cc_tex, GPU_LINEAR, GPU_LINEAR);
        memset(cc_tex.data, 0, 512 * 128 * 2);
        cc_tex_ready = true;
    }
    bool dirty = false;
    int loads = 0;
    for (int k = 0; k < CC_PAGE; k++) {
        int i = cc_page * CC_PAGE + k;
        u64 hash = i < v->count ? v->photos[i].hash : 0;
        int w = hash ? photo_cache_width(hash) : 0;
        if (hash == cc_slot_hash[k] && w == cc_slot_w[k]) continue;
        if (loads++ >= 2) break; // a couple a frame: they may come from the SD card
        cc_slot_hash[k] = hash;
        cc_slot_w[k] = w;
        int sw, sh;
        cc_slot_ok[k] = w > 0 && photo_cache_get(hash, &sw, &sh, thumb_px, 320 * 240);
        if (cc_slot_ok[k]) {
            cc_fill_slot(k, sw, sh);
            dirty = true;
        }
    }
    if (dirty) C3D_TexFlush(&cc_tex);
}

static int cc_pages(const CcView* v) { return v->count > 0 ? (v->count + CC_PAGE - 1) / CC_PAGE : 1; }

static void cc_photo_rect(int k, int* x, int* y) {
    *x = CC_X0 + (k % CC_COLS) * (CC_TW + CC_GAP);
    *y = CC_Y0 + (k / CC_COLS) * (CC_TH + 6);
}

// Names as the game gives them in Camera Check (its PKMN Signs are "Sign").
static const char* cc_name(int species) {
    if (species > 0 && species <= 151) return pokemon_names[species];
    if (species == 9999) return "";
    if (species >= 1004 && species < 1036) return "Sign";
    return "???";
}

static void cc_set_note(const char* s) {
    snprintf(cc_note, sizeof(cc_note), "%s", s);
    cc_note_frames = 150;
}

static void cc_status(const CcView* v, int sel, char* out, size_t size) {
    if (cc_note_frames > 0) {
        snprintf(out, size, "%s", cc_note);
        return;
    }
    if (sel < 0 || sel >= v->count) {
        out[0] = 0;
        return;
    }
    const CcPhoto* p = &v->photos[sel];
    const char* name = cc_name(p->species);
    if (p->marks & CC_NO_POKEMON) snprintf(out, size, "Umm... It looks like there are no Pokemon.");
    else if (!(p->marks & CC_OAK_OK)) snprintf(out, size, "What's this? It's kind of odd...");
    else if (p->marks & CC_OAK) snprintf(out, size, "I have decided to show %s's picture.", name);
    else {
        int n = 0;
        for (int i = 0; i < v->count; i++) n += v->photos[i].species == p->species;
        snprintf(out, size, "%d picture%s of %s %s been taken!", n, n == 1 ? "" : "s", name, n == 1 ? "has" : "have");
    }
}

static void cc_button(int index, int sheet_index, bool usable) {
    C2D_ImageTint tint;
    float x = cc_btn_x[index], y = CC_BTN_Y;
    bool lit = cc_flash == index && cc_flash_frames > 0;
    if (lit) C2D_PlainImageTint(&tint, C2D_Color32(0xFF, 0xE8, 0x80, 0xFF), 0.35f);
    else C2D_PlainImageTint(&tint, C2D_Color32(0x10, 0x10, 0x20, 0xFF), 0.5f);
    C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, sheet_index), x, y, 0.4f, lit || !usable ? &tint : NULL, 1.0f, 1.0f);
}

static void draw_camera_check(CcMode mode) {
    CcView v;
    cc_view(&v);
    int sel = cc_selected();
    u32 white = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF), dim = C2D_Color32(0xC8, 0xD8, 0xE0, 0xFF);
    // Camera Check's background tile.
    C2D_Image tile = C2D_SpriteSheetGetImage(sheet, SHEET_CC_TILE);
    for (int y = 0; y < 240; y += 16) {
        for (int x = 0; x < 320; x += 16) C2D_DrawImageAt(tile, x, y, 0.1f, NULL, 1.0f, 1.0f);
    }
    if (cc_flash_frames > 0) cc_flash_frames--;
    if (cc_note_frames > 0) cc_note_frames--;

    if (mode == CC_CONFIRM) {
        // The game's question (its top screen shows the marked pictures), answered here.
        bool oak = false;
        for (int i = 0; i < v.count; i++) oak |= (v.photos[i].marks & CC_OAK) != 0;
        const char* q = !v.marked ? "No pictures to show to Prof. Oak?" : oak ? "Show this picture to Prof. Oak?" : "Save this picture in the Album?";
        C2D_DrawRectSolid(14, 52, 0.3f, 292, 40, C2D_Color32(0xB0, 0x50, 0x50, 0xE0));
        C2D_DrawRectSolid(16, 54, 0.32f, 288, 36, C2D_Color32(0x80, 0x30, 0x34, 0xE0));
        text(160, 66, 0.5f, white, q, C2D_AlignCenter);
        const char* yes = v.marked ? "Yeah!" : "No.";
        const char* no = v.marked ? "Maybe not..." : "I have some.";
        for (int b = 0; b < 2; b++) {
            float x = b == 0 ? 30 : 170, y = 120;
            C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_BUTTON), x, y, 0.4f, NULL, 1.55f, 1.55f);
            text(x + 39 * 1.55f, y + 9, 0.5f, white, b == 0 ? yes : no, C2D_AlignCenter);
        }
        text(160, 190, 0.45f, dim, "A: yes   B: no", C2D_AlignCenter);
        return;
    }

    if (sel >= 0 && sel < v.count && sel / CC_PAGE != cc_page) cc_page = sel / CC_PAGE;
    if (cc_page >= cc_pages(&v)) cc_page = cc_pages(&v) - 1;
    cc_update_thumbs(&v);

    // Line 1: the chosen photo's Pokemon, room in the album, the page.
    char line[96];
    if (sel >= 0 && sel < v.count) text(10, 5, 0.5f, C2D_Color32(0xFF, 0xE0, 0x60, 0xFF), cc_name(v.photos[sel].species), 0);
    snprintf(line, sizeof(line), "%d/%d", cc_page + 1, cc_pages(&v));
    text(310, 5, 0.5f, white, line, C2D_AlignRight);
    if (v.album_open) {
        snprintf(line, sizeof(line), "Album: %d left", v.album_left > 0 ? v.album_left : 0);
        text(262, 5, 0.5f, dim, line, C2D_AlignRight);
    }

    // The grid: each photo in the game's grey frame, NEW ribbon, its marks; the chosen one in the
    // orange brackets. Photos that can't have Oak's Mark are a little darker (as the game's grid).
    C2D_ImageTint darker;
    C2D_PlainImageTint(&darker, C2D_Color32(0, 0, 0, 0xFF), 0.35f);
    for (int k = 0; k < CC_PAGE; k++) {
        int i = cc_page * CC_PAGE + k;
        if (i >= v.count) break;
        const CcPhoto* p = &v.photos[i];
        int x, y;
        cc_photo_rect(k, &x, &y);
        C2D_DrawRectSolid(x - 2, y - 2, 0.3f, CC_TW + 4, CC_TH + 4, C2D_Color32(0xC8, 0xC8, 0xC8, 0xFF));
        if (cc_tex_ready && cc_slot_ok[k]) {
            Tex3DS_SubTexture sub = { CC_TW, CC_TH, (k % 8) * CC_TW / 512.0f, 1.0f - (k / 8) * CC_TH / 128.0f,
                                      (k % 8 + 1) * CC_TW / 512.0f, 1.0f - (k / 8 + 1) * CC_TH / 128.0f };
            C2D_Image img = { &cc_tex, &sub };
            C2D_DrawImageAt(img, x, y, 0.35f, (p->marks & CC_OAK_OK) ? NULL : &darker, 1.0f, 1.0f);
        } else {
            C2D_DrawRectSolid(x, y, 0.35f, CC_TW, CC_TH, C2D_Color32(0x20, 0x30, 0x38, 0xFF));
        }
        if (p->marks & CC_NEW) C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_CC_NEW), x + CC_TW - 8, y - 1, 0.4f, NULL, 1.0f, 1.0f);
        if (p->marks & CC_ALBUM) C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_CC_MARK_ALBUM), x + 2, y + CC_TH - 14, 0.4f, NULL, 1.0f, 1.0f);
        if (p->marks & CC_OAK) C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_CC_MARK_OAK), x + CC_TW - 14, y + CC_TH - 14, 0.4f, NULL, 1.0f, 1.0f);
        if (i == sel) {
            float l = x - 5, t = y - 5, r = x + CC_TW + 5, b = y + CC_TH + 5;
            C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_FOCUS_TL), l, t, 0.5f, NULL, 1.0f, 1.0f);
            C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_FOCUS_TR), r - 12, t, 0.5f, NULL, 1.0f, 1.0f);
            C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_FOCUS_BL), l, b - 10, 0.5f, NULL, 1.0f, 1.0f);
            C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_FOCUS_BR), r - 12, b - 10, 0.5f, NULL, 1.0f, 1.0f);
        }
    }
    // Page knobs, where there's a page to turn to.
    if (cc_page > 0) C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_CC_KNOB_L), 0, CC_KNOB_Y, 0.4f, NULL, 1.0f, 1.0f);
    if (cc_page < cc_pages(&v) - 1) C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_CC_KNOB_R), 296, CC_KNOB_Y, 0.4f, NULL, 1.0f, 1.0f);

    // Line 2: as the game's message box would say.
    cc_status(&v, sel, line, sizeof(line));
    C2D_DrawRectSolid(6, 186, 0.3f, 308, 20, C2D_Color32(0xB0, 0x50, 0x50, 0xD0));
    text(12, 190, 0.5f, white, line, 0);

    const CcPhoto* p = sel >= 0 && sel < v.count ? &v.photos[sel] : NULL;
    cc_button(0, SHEET_CC_OAK, p && (p->marks & CC_OAK_OK));
    cc_button(1, SHEET_CC_ALBUM, p && v.album_open);
    cc_button(2, SHEET_CC_SEND, true);
}

static void cc_turn(const CcView* v, int dir) {
    int page = cc_page + dir;
    if (page < 0 || page >= cc_pages(v)) return;
    cc_page = page;
    cc_select(page * CC_PAGE);
}

static void cc_mark(const CcView* v, int sel, int button) {
    cc_flash = button;
    cc_flash_frames = 8;
    if (sel < 0 || sel >= v->count) return;
    const CcPhoto* p = &v->photos[sel];
    if (button == 0) {
        if (!(p->marks & CC_OAK_OK)) cc_set_note(p->marks & CC_NO_POKEMON ? "No Pokemon are in this picture." : "Mmm? This is not clear.");
        cc_toggle_oak(sel);
    } else {
        if (!v->album_open) cc_set_note("First choose the pictures with Oak's Mark.");
        else if (!(p->marks & CC_ALBUM) && v->album_left <= 0) cc_set_note("Delete \"Album Marks\" to save other pictures.");
        cc_toggle_album(sel);
    }
}

static void cc_input(CcMode mode, u32 down, const touchPosition* t) {
    CcView v;
    cc_view(&v);
    if (mode == CC_CONFIRM) {
        if (!(down & KEY_TOUCH)) return; // A and B reach the game itself
        if (inside(t, 30, 120, 121, 33)) cc_answer(true);
        if (inside(t, 170, 120, 121, 33)) cc_answer(false);
        return;
    }
    int sel = cc_selected();
    if (down & KEY_TOUCH) {
        for (int k = 0; k < CC_PAGE; k++) {
            int x, y, i = cc_page * CC_PAGE + k;
            cc_photo_rect(k, &x, &y);
            if (i < v.count && inside(t, x - 2, y - 2, CC_TW + 4, CC_TH + 5)) cc_select(i);
        }
        if (inside(t, 0, CC_KNOB_Y - 10, 24, 59)) cc_turn(&v, -1);
        if (inside(t, 296, CC_KNOB_Y - 10, 24, 59)) cc_turn(&v, 1);
        for (int b = 0; b < 3; b++) {
            if (!inside(t, cc_btn_x[b], CC_BTN_Y - 3, CC_BTN_W, CC_BTN_H + 6)) continue;
            if (b == 2) {
                cc_flash = 2;
                cc_flash_frames = 8;
                cc_send();
            } else {
                cc_mark(&v, sel, b);
            }
        }
        return;
    }
    // Buttons: the D-Pad / Circle Pad choose, L / R turn pages, A Oak's Mark, Y Album Mark, START to
    // Prof. Oak.
    int to = sel;
    if (down & KEY_LEFT) to = sel - 1;
    if (down & KEY_RIGHT) to = sel + 1;
    if (down & KEY_UP) to = sel - CC_COLS;
    if (down & KEY_DOWN) to = sel + CC_COLS;
    if (to != sel && to >= 0 && to < v.count) cc_select(to);
    if (down & KEY_L) cc_turn(&v, -1);
    if (down & KEY_R) cc_turn(&v, 1);
    if (down & KEY_A) cc_mark(&v, sel, 0);
    if (down & KEY_Y) cc_mark(&v, sel, 1);
    if (down & KEY_START) {
        cc_flash = 2;
        cc_flash_frames = 8;
        cc_send();
    }
}

// --- Oak's Lab -------------------------------------------------------------------------------------

// Settings panels (one per setting), each with ON / OFF buttons: the lab menu's button without its
// flag tab (gfx/button.png, 78x21, label box x 8-69, y 3-16).
#define PANEL_X 12
#define PANEL_W 296
#define PANEL_H 58
#define PANEL_Y0 52
#define PANEL_STEP 66
#define ON_X (PANEL_X + 132)
#define OFF_X (PANEL_X + 132 + 78 + 6)
#define BUTTON_W 78
#define BUTTON_H 21

static const struct {
    Setting setting;
    const char* title;
    const char* hint;
} panels[2] = {
    { SETTING_GYRO, "Gyro aiming", "Turn to aim" },
    { SETTING_WIDESCREEN, "Widescreen", "Fill the screen" },
};

static int panel_y(int i) { return PANEL_Y0 + i * PANEL_STEP; }
static int switch_y(int i) { return panel_y(i) + PANEL_H - BUTTON_H - 6; } // below the title, right

// A lab-menu style button; the selected one framed by the game's orange brackets, the other dimmed.
static void game_button(float x, float y, const char* label, bool selected) {
    C2D_ImageTint dim;
    C2D_PlainImageTint(&dim, C2D_Color32(0, 0, 0, 0xFF), 0.45f);
    C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_BUTTON), x, y, 0.4f, selected ? NULL : &dim, 1.0f, 1.0f);
    text(x + 39, y + 5, 0.5f, selected ? C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF) : C2D_Color32(0x90, 0x90, 0xA8, 0xFF), label, C2D_AlignCenter);
    if (!selected) return;
    float l = x + 8 - 5, t = y + 3 - 5, r = x + 70 + 5, b = y + 17 + 5; // just outside the label box
    C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_FOCUS_TL), l, t, 0.5f, NULL, 1.0f, 1.0f);
    C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_FOCUS_TR), r - 12, t, 0.5f, NULL, 1.0f, 1.0f);
    C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_FOCUS_BL), l, b - 10, 0.5f, NULL, 1.0f, 1.0f);
    C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_FOCUS_BR), r - 12, b - 10, 0.5f, NULL, 1.0f, 1.0f);
}

static void draw_lab(void) {
    u32 white = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF), dim = C2D_Color32(0xC8, 0xD8, 0xC8, 0xFF);
    // The lab's interior (gfx/lab.png: the game's lab background, from its texture strips), dimmed.
    C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_LAB), 0, 0, 0.1f, NULL, 1.0f, 1.0f);
    C2D_DrawRectSolid(0, 0, 0.15f, 320, 240, C2D_Color32(0x08, 0x10, 0x18, 0x98));
    film_title("Professor Oak's Lab");

    // Settings: see-through panels (the lab shows through), light edge like the game's text boxes.
    for (int p = 0; p < 2; p++) {
        int y = panel_y(p);
        C2D_DrawRectSolid(PANEL_X, y, 0.3f, PANEL_W, PANEL_H, C2D_Color32(0xD8, 0xE8, 0xD8, 0xC0));
        C2D_DrawRectSolid(PANEL_X + 2, y + 2, 0.35f, PANEL_W - 4, PANEL_H - 4, C2D_Color32(0x24, 0x50, 0x46, 0x90));
        text(PANEL_X + 10, y + 5, 0.75f, white, panels[p].title, 0);
        text(PANEL_X + 10, y + 37, 0.45f, dim, panels[p].hint, 0);
        bool on = setting(panels[p].setting);
        game_button(ON_X, switch_y(p), "ON", on);
        game_button(OFF_X, switch_y(p), "OFF", !on);
    }

    text(160, 200, 0.45f, dim, "Tap ON or OFF.  START+SELECT: exit", C2D_AlignCenter);
    text(314, 224, 0.4f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xC0), "v" SNAP_VERSION, C2D_AlignRight);
}

// --- Title (boot, intro, title screen, main menu) -----------------------------------------------------

static void draw_title(const char* rom_state, const char* boot_state, const SnapStats* stats) {
    C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_ISLAND), 0, 0, 0.1f, NULL, 1.0f, 1.0f);
    text(314, 224, 0.4f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xC0), "v" SNAP_VERSION, C2D_AlignRight);
    // While the game hasn't started (or doesn't), what the front end knows: a stall at boot would
    // otherwise show only this picture and a black top screen.
    static u64 first_tick;
    if (!first_tick) first_tick = svcGetSystemTick();
    bool booted = strcmp(boot_state, "running") == 0;
    bool late = svcGetSystemTick() - first_tick > SYSCLOCK_ARM11 * 6ull && stats->display_lists < 10;
    if (booted && !late) return;
    u32 white = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF);
    C2D_DrawRectSolid(10, 140, 0.5f, 300, 74, C2D_Color32(0x00, 0x00, 0x00, 0xB0));
    char line[96];
    snprintf(line, sizeof(line), "ROM: %s", rom_state);
    text(18, 146, 0.5f, white, line, 0);
    snprintf(line, sizeof(line), "Game: %s%s", boot_state, booted ? " (no picture yet)" : "");
    text(18, 162, 0.5f, white, line, 0);
    snprintf(line, sizeof(line), "VI %u  display lists %u  audio %u", stats->frames, stats->display_lists, stats->audio_tasks);
    text(18, 178, 0.5f, white, line, 0);
    text(18, 194, 0.5f, white, "Log: sdmc:/3ds/psnap/runtime.log", 0);
}

// --- ROM pages (rom_check.c): checking it the first time, or none usable ------------------------------

static void rom_page(const char* title) {
    C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_ISLAND), 0, 0, 0.1f, NULL, 1.0f, 1.0f);
    C2D_DrawRectSolid(0, 0, 0.2f, 320, 240, C2D_Color32(0x00, 0x00, 0x00, 0x90));
    film_title(title);
    text(314, 224, 0.4f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xC0), "v" SNAP_VERSION, C2D_AlignRight);
}

void bottom_ui_draw_rom_progress(const char* message, int percent) {
    u32 white = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF);
    rom_page("Checking the ROM");
    text(160, 92, 0.45f, white, message, C2D_AlignCenter);
    C2D_DrawRectSolid(40, 112, 0.5f, 240, 14, C2D_Color32(0x20, 0x20, 0x20, 0xE0));
    C2D_DrawRectSolid(42, 114, 0.5f, 236 * percent / 100, 10, C2D_Color32(0xF0, 0x90, 0x20, 0xFF));
    text(160, 140, 0.45f, C2D_Color32(0xC0, 0xC8, 0xD8, 0xFF), "Only the first time.", C2D_AlignCenter);
}

void bottom_ui_draw_rom_help(const char* state, const char* found) {
    u32 white = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF), dim = C2D_Color32(0xC0, 0xC8, 0xD8, 0xFF);
    u32 warn = C2D_Color32(0xFF, 0xC0, 0x60, 0xFF);
    rom_page("Pokemon Snap ROM");
    int y = 62;
    text(12, y, 0.45f, warn, state, 0);
    if (found && found[0]) text(12, y += 16, 0.45f, dim, found, 0);
    text(12, y += 24, 0.45f, white, "Put your own dump of Pokemon Snap (USA) in", 0);
    text(24, y += 16, 0.45f, white, "sdmc:/3ds/psnap/   (or sdmc:/roms/n64/)", 0);
    text(12, y += 20, 0.45f, dim, "Any file name. .z64, .v64 or .n64. 16 MB.", 0);
    text(12, y += 16, 0.45f, dim, "SHA-1 of the right one (as .z64):", 0);
    text(12, y += 16, 0.45f, dim, ROM_SHA1_USA, 0);
    text(12, 210, 0.45f, white, "Then start the game again.  START+SELECT: exit", 0);
}

// The top screen while there's no usable ROM: Pokemon Island (stretched to 400 wide, cropped top and
// bottom) and a pointer to the bottom screen's instructions, in the game's font.
void bottom_ui_draw_rom_top(const char* state) {
    u32 white = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF), warn = C2D_Color32(0xFF, 0xC0, 0x60, 0xFF);
    C2D_DrawImageAt(C2D_SpriteSheetGetImage(sheet, SHEET_ISLAND), 0, -30, 0.1f, NULL, 1.25f, 1.25f);
    C2D_DrawRectSolid(0, 0, 0.2f, 400, 240, C2D_Color32(0x00, 0x00, 0x00, 0x90));
    text(200, 70, 0.7f, white, "Pokemon Snap", C2D_AlignCenter);
    text(200, 100, 0.7f, white, "ROM needed", C2D_AlignCenter);
    text(200, 146, 0.45f, warn, state, C2D_AlignCenter);
    text(200, 170, 0.45f, white, "See the bottom screen for what to do.", C2D_AlignCenter);
    text(392, 224, 0.4f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xC0), "v" SNAP_VERSION, C2D_AlignRight);
}

// --- Status page (other scenes) ----------------------------------------------------------------

static void draw_status(const char* rom_state, const char* boot_state, const SnapStats* stats) {
    u32 white = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF), dim = C2D_Color32(0xA8, 0xB8, 0xD0, 0xFF);
    C2D_DrawRectSolid(0, 0, 0.4f, 320, 36, C2D_Color32(0xD8, 0x30, 0x30, 0xFF));
    text(12, 8, 0.75f, white, "Pokemon Snap 3DS", 0);
    text(250, 13, 0.5f, white, "v" SNAP_VERSION, 0);
    char line[96];
    snprintf(line, sizeof(line), "ROM: %s", rom_state);
    text(12, 50, 0.5f, dim, line, 0);
    snprintf(line, sizeof(line), "Game: %s", boot_state);
    text(12, 70, 0.5f, dim, line, 0);
    snprintf(line, sizeof(line), "Frames %u   display lists %u   audio %u", stats->frames, stats->display_lists, stats->audio_tasks);
    text(12, 90, 0.5f, dim, line, 0);
    text(12, 212, 0.45f, dim, "START+SELECT: exit", 0);
}

// --- Entry points ------------------------------------------------------------------------------------

void bottom_ui_init(void) {
    sheet = C2D_SpriteSheetLoadFromMem(ui_t3x, ui_t3x_size);
    // Pixel art (font, film, icons at whole multiples): no smoothing.
    C3D_TexSetFilter(C2D_SpriteSheetGetImage(sheet, SHEET_FONT).tex, GPU_NEAREST, GPU_NEAREST);
}

void bottom_ui_exit(void) {
    if (best.tex_ready) C3D_TexDelete(&best.tex);
    if (cc_tex_ready) C3D_TexDelete(&cc_tex);
    C2D_SpriteSheetFree(sheet);
}

void bottom_ui_input(u32 down, const touchPosition* touch) {
    if (tap_frames > 0) tap_frames--;
    CcMode cc = cc_view(NULL);
    if (cc != CC_OFF) {
        cc_input(cc, down, touch);
        return;
    }
    if (!(down & KEY_TOUCH)) return;
    SnapScene scene = snap_scene();
    if (scene == SNAP_SCENE_LAB) {
        for (int p = 0; p < 2; p++) {
            Setting st = panels[p].setting;
            if (inside(touch, ON_X, switch_y(p), BUTTON_W, BUTTON_H) && !setting(st)) setting_set(st, true);
            if (inside(touch, OFF_X, switch_y(p), BUTTON_W, BUTTON_H) && setting(st)) setting_set(st, false);
        }
    }
    if (scene == SNAP_SCENE_COURSE) {
        for (int i = 0; i < 4; i++) {
            if (!inside(touch, items[i].x, items[i].y, ITEM_SIZE, ITEM_SIZE) || !has_item(&items[i])) continue;
            tap_keys = items[i].key;
            tap_frames = 6; // ~100 ms: the game (30 fps) sees it pressed for a few frames
            tapped_item = i;
            fprintf(stderr, "[bottom] tapped %s\n", items[i].label);
        }
    }
}

u32 bottom_ui_keys(void) { return tap_frames > 0 && snap_scene() == SNAP_SCENE_COURSE ? tap_keys : 0; }

void bottom_ui_draw(const char* rom_state, const char* boot_state, const SnapStats* stats) {
    SnapScene scene = snap_scene();
    CcMode cc = cc_view(NULL);
    if (cc != CC_OFF) {
        best.course = -1;
        draw_camera_check(cc);
    } else if (scene == SNAP_SCENE_COURSE) {
        update_best();
        draw_course();
    } else if (scene == SNAP_SCENE_TITLE) {
        draw_title(rom_state, boot_state, stats);
    } else if (scene == SNAP_SCENE_LAB) {
        best.course = -1; // looked up again in the next course (the report may have a new best)
        draw_lab();
    } else {
        best.course = -1;
        draw_status(rom_state, boot_state, stats);
    }
}
