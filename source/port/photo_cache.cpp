// Photo thumbnails for the bottom screen. Pokemon Snap keeps photos as scene data (PhotoData) and
// re-renders them whenever it shows one; its CPU then averages the 2x render into a sprite bitmap
// (window/847B60.c, func_80374714_847EC4). Hooks in that function (pokemonsnap.us.3ds.toml) catch the
// 2x render itself (twice the sprite's size each way: 104x78 for Camera Check's grid, 280x210 for its
// big view) - or the finished bitmap if that's missing - kept under a hash of the PhotoData and
// written to DATA_PATH/photos/ by the watchdog thread, so the course's best photo (save data) can be
// shown later.

#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <set>
#include <mutex>
#include <vector>

#include "photo_cache.h"
#include "recomp.h"

namespace {

struct Thumb {
    uint16_t w = 0, h = 0;
    std::vector<uint16_t> px; // RGBA5551, rows top to bottom
    bool dirty = false;
};

std::mutex cache_mutex;
std::map<uint64_t, Thumb> cache;
std::set<uint64_t> missing; // not on the SD card either (photo_cache_width), until captured
std::string dir;

uint32_t pending_photo, pending_sprite; // from the hook at the function's start (game thread)
uint32_t pending_render;                // { u16* buf; s32 stride (pixels) }, from func_80374608

// RDRAM ranges of the sprite bitmaps photos were printed into (newest kept), for stereoscopic 3D:
// the renderer draws rectangles textured from them at a depth of their own (photo_is_bitmap).
struct Range { uint32_t start, end; };
std::mutex range_mutex;
Range ranges[128];
unsigned range_next;

void add_range(uint32_t start, uint32_t bytes) {
    start &= 0x00FFFFFF;
    std::lock_guard lock{ range_mutex };
    for (const Range& r : ranges) {
        if (r.start == start && r.end == start + bytes) return;
    }
    ranges[range_next++ % 128] = { start, start + bytes };
}

uint32_t rd32(const uint8_t* rdram, uint32_t a) { return *reinterpret_cast<const uint32_t*>(rdram + (a & 0x00FFFFFC)); }
uint16_t rd16(const uint8_t* rdram, uint32_t a) { return *reinterpret_cast<const uint16_t*>(rdram + ((a & 0x00FFFFFE) ^ 2)); }
int16_t rds16(const uint8_t* rdram, uint32_t a) { return (int16_t)rd16(rdram, a); }

std::string path_of(uint64_t hash) {
    char name[32];
    snprintf(name, sizeof(name), "/%016llX.bin", (unsigned long long)hash);
    return dir + name;
}

bool load_file(uint64_t hash, Thumb& t) {
    FILE* f = fopen(path_of(hash).c_str(), "rb");
    if (!f) return false;
    uint16_t wh[2];
    bool ok = fread(wh, 2, 2, f) == 2 && wh[0] && wh[1] && wh[0] <= 320 && wh[1] <= 240;
    if (ok) {
        t.w = wh[0];
        t.h = wh[1];
        t.px.resize((size_t)t.w * t.h);
        ok = fread(t.px.data(), 2, t.px.size(), f) == t.px.size();
    }
    fclose(f);
    return ok;
}

} // namespace

// PhotoData from offset 8 on (camera, Pokemon, items, effects): the first word's flags change when a
// photo is scored, so they're left out.
extern "C" uint64_t photo_cache_hash(const uint8_t* rdram, uint32_t photo) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t off = 8; off < 0x3A0; off += 4) {
        h ^= rd32(rdram, photo + off);
        h *= 0x100000001B3ull;
    }
    return h;
}

extern "C" void photo_cache_init(const char* data_path) {
    dir = std::string(data_path) + "/photos";
    mkdir(dir.c_str(), 0777);
}

extern "C" void photo_thumb_begin(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    pending_photo = (uint32_t)ctx->r4;
    pending_sprite = (uint32_t)ctx->r5;
    pending_render = 0;
}

// After func_80374608_847DB8, which rendered the photo at twice the sprite's size: its result.
extern "C" void photo_thumb_render(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    pending_render = (uint32_t)ctx->r2;
}

// Sprite: width +0x04, height +0x06, bitmap pointer +0x34. Bitmap (0x10 each): width_img +0x02, buf
// +0x08, actualHeight +0x0C. Rows go through the bitmaps in turn; odd rows have their 32-bit words'
// halves swapped (address XOR 4), as the game writes them for TMEM.
extern "C" void photo_thumb_end(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    uint32_t photo = pending_photo, sprite = pending_sprite, render = pending_render;
    pending_photo = pending_sprite = pending_render = 0;
    if (photo < 0x80000000u || sprite < 0x80000000u) return;
    int w = rds16(rdram, sprite + 0x04), h = rds16(rdram, sprite + 0x06);
    uint32_t bitmap = rd32(rdram, sprite + 0x34);
    if (w <= 0 || h <= 0 || w > 320 || h > 240 || bitmap < 0x80000000u) return;
    for (int y = 0, b = bitmap; y < h && b; b += 0x10) { // each bitmap strip of the photo
        int width_img = rds16(rdram, b + 0x02), actual = rds16(rdram, b + 0x0C);
        if (width_img <= 0 || actual <= 0) break;
        add_range(rd32(rdram, b + 0x08), (uint32_t)(width_img * actual * 2));
        y += actual;
    }
    uint64_t hash = photo_cache_hash(rdram, photo);
    // The 2x render, if there is one and it fits (the render buffer is 280x240 at most).
    uint32_t src = render >= 0x80000000u ? rd32(rdram, render) : 0;
    int stride = render >= 0x80000000u ? (int)rd32(rdram, render + 4) : 0;
    bool big = src >= 0x80000000u && src < 0x80800000u && stride >= w * 2 && stride <= 640 && w * 2 <= 320 && h * 2 <= 240;
    int tw = big ? w * 2 : w, th = big ? h * 2 : h;
    {
        std::lock_guard lock{ cache_mutex };
        auto it = cache.find(hash);
        if (it != cache.end() && it->second.w >= tw) return; // already have it at this size or larger
    }
    Thumb t;
    t.w = (uint16_t)tw;
    t.h = (uint16_t)th;
    t.px.resize((size_t)tw * th);
    if (big) {
        for (int y = 0; y < th; y++) {
            for (int x = 0; x < tw; x++) t.px[(size_t)y * tw + x] = rd16(rdram, src + (uint32_t)(y * stride + x) * 2);
        }
        t.dirty = true;
        std::lock_guard lock{ cache_mutex };
        missing.erase(hash);
        cache[hash] = std::move(t);
        return;
    }
    int row = 0;
    for (int y = 0; y < h; y++) {
        int width_img = rds16(rdram, bitmap + 0x02), actual = rds16(rdram, bitmap + 0x0C);
        uint32_t buf = rd32(rdram, bitmap + 0x08);
        for (int x = 0; x < w; x++) {
            uint32_t a = buf + (uint32_t)(row * width_img + x) * 2;
            if (row & 1) a ^= 4;
            t.px[(size_t)y * w + x] = rd16(rdram, a);
        }
        if (++row >= actual) {
            row = 0;
            bitmap += 0x10;
        }
    }
    t.dirty = true;
    std::lock_guard lock{ cache_mutex };
    missing.erase(hash);
    cache[hash] = std::move(t);
}

extern "C" bool photo_is_bitmap(uint32_t phys) {
    std::lock_guard lock{ range_mutex };
    for (const Range& r : ranges) {
        if (phys >= r.start && phys < r.end) return true;
    }
    return false;
}

extern "C" void photo_bitmaps_clear(void) {
    std::lock_guard lock{ range_mutex };
    for (Range& r : ranges) r = {};
    range_next = 0;
}

extern "C" void photo_cache_flush(void) {
    std::vector<std::pair<uint64_t, Thumb>> out;
    {
        std::lock_guard lock{ cache_mutex };
        for (auto& [hash, t] : cache) {
            if (!t.dirty) continue;
            t.dirty = false;
            out.emplace_back(hash, t);
        }
    }
    for (auto& [hash, t] : out) {
        FILE* f = fopen(path_of(hash).c_str(), "wb");
        if (!f) continue;
        uint16_t wh[2] = { t.w, t.h };
        fwrite(wh, 2, 2, f);
        fwrite(t.px.data(), 2, t.px.size(), f);
        fclose(f);
    }
}

extern "C" bool photo_cache_get(uint64_t hash, int* w, int* h, uint16_t* pixels, int max_pixels) {
    Thumb t;
    {
        std::lock_guard lock{ cache_mutex };
        auto it = cache.find(hash);
        if (it != cache.end()) t = it->second;
    }
    if (t.px.empty()) {
        if (!load_file(hash, t)) return false;
        std::lock_guard lock{ cache_mutex };
        cache.emplace(hash, t);
    }
    if ((int)t.px.size() > max_pixels) return false;
    *w = t.w;
    *h = t.h;
    memcpy(pixels, t.px.data(), t.px.size() * 2);
    return true;
}

extern "C" int photo_cache_width(uint64_t hash) {
    {
        std::lock_guard lock{ cache_mutex };
        auto it = cache.find(hash);
        if (it != cache.end()) return it->second.w;
        if (missing.count(hash)) return 0;
    }
    Thumb t;
    bool found = load_file(hash, t);
    std::lock_guard lock{ cache_mutex };
    if (!found) {
        missing.insert(hash);
        return 0;
    }
    auto it = cache.emplace(hash, std::move(t)).first;
    return it->second.w;
}
