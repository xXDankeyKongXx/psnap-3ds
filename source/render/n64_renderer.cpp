// F3DEX2/RDP display-list interpreter for the 3DS port. See n64_renderer.hpp.
//
// Model (similar to the PC "Fast3D" renderers): RSP work (matrices, T&L, culling) is done on the
// CPU here; RDP state is recorded per draw call and turned into PICA200 state by c3d_backend.cpp.
// Texture loads are tracked by TMEM address and decoded straight from RDRAM, not via a TMEM copy.

#include "n64_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <chrono>
#include <thread>

#include "gbi.h"
#include "subtitle_rgba.h"
#include "dankey_logo_rgba.h"
#include "dankey_copyright_rgba.h"
#include "cc_parts_rgba.h"
#include "camera_check.h"
#include "photo_cache.h"
extern "C" {
#include <3ds/types.h>
#include <3ds/svc.h>
}

// Development: which part of the interpreter is running, sampled by the profiler thread
// (snap_game.cpp) - a plain store, so it doesn't distort the timing like clock reads did.
extern "C" { volatile int n64_gfx_phase; }
// Development capture (pokes.txt "isolate"): published frames keep only draws textured from RDRAM
// [n64_isolate_lo, n64_isolate_hi), zoomed about a fixed point, on a magenta background; used to
// record a character's model and animation from the game itself.
extern "C" { volatile uint32_t n64_isolate_lo, n64_isolate_hi; }
extern "C" { extern volatile bool snap_boot_skip; } // snap_game.cpp
extern "C" { extern volatile uint32_t snap_hud_gauge_passes, snap_hud_map_passes; } // snap_patches.cpp
enum { PHASE_IDLE, PHASE_CMD, PHASE_VTX, PHASE_TRI, PHASE_TEX_HASH, PHASE_TEX_DECODE, PHASE_STATE };
struct Phase {
    int prev;
    explicit Phase(int p) : prev(n64_gfx_phase) { n64_gfx_phase = p; }
    ~Phase() { n64_gfx_phase = prev; }
};

// Development: texture-binding counters, summed over display lists.
static uint32_t tex_binds, tex_memo_hits, tex_cache_hits, tex_decodes;
extern "C" { volatile int n64_texlog_remaining; } // development: log the next N texture decodes (pokes "texlog N")
extern "C" { volatile bool n64_tex_nocache; }     // development: hash every texture bind in full (pokes "texnocache 1")
extern "C" { volatile bool n64_debug_nocull; }    // development: no back/front face culling (pokes "nocull 1")
// Widescreen (settings.c): on the framebuffers the VI shows, full-width scissors and fills reach the
// top screen's whole 400 pixels instead of the middle 320. The game's 4:3 projection already puts
// what lies beyond its view's edges there (x/w past +-1), at the right proportions; 2D art keeps 4:3.
extern "C" { extern volatile bool n64_widescreen; }
// Widescreen in a course (set each frame by the front end): the HUD's top-right corner (the film
// counter) moves out to the screen's corner.
extern "C" { volatile bool n64_hud_wide; }
// Stereoscopic 3D in the Album and the Camera Check (set each frame by the front end): photos sit at
// a depth of their own behind the screen (DrawCmd::photo_plane, c3d_backend.cpp).
extern "C" { volatile bool n64_photo_depth; }
// The title screen is showing (main.c): the logo gets the port's subtitle (add_subtitle).
extern "C" { volatile bool n64_title_screen; }
extern "C" bool photo_is_bitmap(uint32_t phys); // photo_cache.cpp
extern "C" { volatile int n64_dltrace_dl = -1; }  // development: log every command of this display list number
static uint64_t tex_hashed_bytes;
extern "C" void n64_renderer_profile_dump(unsigned dl_count) {
    fprintf(stderr, "[perf] textures per DL:");
    fprintf(stderr, " binds %lu memo %lu cache %lu decodes %lu hashedKB %llu", (unsigned long)(tex_binds / dl_count),
        (unsigned long)(tex_memo_hits / dl_count), (unsigned long)(tex_cache_hits / dl_count), (unsigned long)(tex_decodes / dl_count),
        (unsigned long long)(tex_hashed_bytes / dl_count / 1024));
    tex_binds = tex_memo_hits = tex_cache_hits = tex_decodes = 0;
    tex_hashed_bytes = 0;
    fprintf(stderr, "\n");
}

namespace n64r {

namespace {

constexpr uint32_t rdram_limit = 9 * 1024 * 1024;
constexpr int vertex_buffer_size = 64;
constexpr int matrix_stack_size = 32;
constexpr int dl_stack_size = 18;
constexpr size_t max_frame_vertices = 200000;
constexpr size_t texture_cache_limit = 2048; // decoded textures kept (a busy area binds ~500 per frame)
// Memory for decoded textures and palette indices. Counts alone let them grow to tens of MB in big
// areas (Cardon Forest), and on a console the app ran out of memory (std::bad_alloc) there.
constexpr size_t texture_cache_budget = 8 * 1024 * 1024;
constexpr size_t index_cache_budget = 2 * 1024 * 1024;

// The N64 framebuffer (fb_w x fb_h) is shown 4:3 pillarboxed on the 400x240 top screen.
constexpr float top_w = 400.0f, top_h = 240.0f;

// --- Pixel/texture helpers ----------------------------------------------------------------------

inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8) | a;
}

inline uint8_t c5to8(uint32_t c) { return (uint8_t)((c << 3) | (c >> 2)); }

inline uint32_t rgba5551(uint16_t c) {
    return rgba(c5to8((c >> 11) & 31), c5to8((c >> 6) & 31), c5to8((c >> 1) & 31), (c & 1) ? 255 : 0);
}

inline uint32_t ia16(uint16_t c) {
    uint8_t i = c >> 8, a = c & 0xFF;
    return rgba(i, i, i, a);
}

inline uint32_t next_pow2(uint32_t v) {
    uint32_t p = 8;
    while (p < v && p < 1024) p <<= 1;
    return p;
}

// Offset of texel (x, y) in an 8x8-Morton-tiled PICA200 texture of width w. PICA200 textures are
// stored bottom row first, so y is flipped here and UV v is flipped when vertices are emitted.
inline uint32_t tiled_offset(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    y = h - 1 - y;
    uint32_t tile = (y >> 3) * (w >> 3) + (x >> 3);
    uint32_t morton = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return tile * 64 + morton;
}

uint64_t fnv1a(uint64_t h, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

// Hash for texture source bytes, which are hashed on every texture bind (~120 KB per gameplay frame).
// Byte-wise 64-bit FNV took over half of the display-list time; this mixes 8 bytes per step in two
// independent 32-bit lanes (two cheap multiplies), keeping a 64-bit result.
inline uint32_t rotl32(uint32_t x, int r) { return (x << r) | (x >> (32 - r)); }

// Small key from a few 32-bit words (texture parameters).
inline uint64_t mix_words(const uint32_t* w, size_t n) {
    uint32_t a = 0x9E3779B1u, b = 0x85EBCA77u;
    for (size_t i = 0; i < n; i++) {
        a = rotl32((a ^ w[i]) * 0x9E3779B1u, 13);
        b = rotl32((b + w[i]) * 0xC2B2AE3Du, 17) ^ a;
    }
    return (uint64_t)b << 32 | a;
}
uint64_t hash_bytes(uint64_t seed, const uint8_t* p, size_t n) {
    uint32_t a = (uint32_t)seed ^ 0x9E3779B1u, b = (uint32_t)(seed >> 32) ^ 0x85EBCA77u;
    while (n >= 8) {
        uint32_t w0, w1;
        memcpy(&w0, p, 4);
        memcpy(&w1, p + 4, 4);
        a = rotl32((a ^ w0) * 0x9E3779B1u, 13);
        b = rotl32((b ^ w1) * 0x85EBCA77u, 17);
        p += 8;
        n -= 8;
    }
    while (n--) a = rotl32((a ^ *p++) * 0xC2B2AE3Du, 11);
    a ^= b * 0x27D4EB2Fu; // cross-mix the lanes
    b ^= a * 0x165667B1u;
    return (uint64_t)b << 32 | a;
}

struct Mat4 {
    float m[4][4];
};

Mat4 identity() {
    Mat4 r{};
    for (int i = 0; i < 4; i++) r.m[i][i] = 1.0f;
    return r;
}

// Row-vector convention (v' = v * M), as on the N64.
Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        }
    }
    return r;
}

struct Vertex {
    float x, y, z, w;   // 3DS clip space (before the top-screen rotation), viewport applied
    float u, v;         // texel coordinates after the G_TEXTURE scale
    uint8_t color[4];
    float screen_z;     // the RSP's screen depth (0..1022 for the default viewport), for G_BRANCH_Z
};

struct Tile {
    uint8_t fmt = 0, siz = 0, palette = 0;
    uint8_t cms = 0, cmt = 0, masks = 0, maskt = 0, shifts = 0, shiftt = 0;
    uint16_t line = 0, tmem = 0;
    uint16_t uls = 0, ult = 0, lrs = 0, lrt = 0; // 10.2 fixed point
};

struct TmemLoad {
    uint32_t src = 0;         // RDRAM physical address of the first loaded texel
    uint32_t src_stride = 0;  // LOADTILE: bytes per source row; 0 for LOADBLOCK (contiguous)
    uint32_t tmem_line = 0;   // LOADTILE: bytes per row in TMEM
    // LOADBLOCK with dxt 0: the RDP didn't swap odd rows while loading, but still un-swaps them when
    // sampling (64-bit words, halves exchanged), so the image's odd rows are stored pre-swapped in
    // RDRAM (Pokemon Snap's logos). The decoder reads them with byte offsets XOR 4.
    bool odd_rows_swapped = false;
};

struct Light {
    float col[3];
    float dir[3];
};

const CcInput cc_rgb_a[16] = { CC_COMBINED, CC_TEX0, CC_TEX1, CC_PRIM, CC_SHADE, CC_ENV, CC_ONE, CC_NOISE,
    CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO };
const CcInput cc_rgb_b[16] = { CC_COMBINED, CC_TEX0, CC_TEX1, CC_PRIM, CC_SHADE, CC_ENV, CC_CENTER, CC_K4,
    CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO };
const CcInput cc_rgb_c[32] = { CC_COMBINED, CC_TEX0, CC_TEX1, CC_PRIM, CC_SHADE, CC_ENV, CC_SCALE, CC_COMBINED_A,
    CC_TEX0_A, CC_TEX1_A, CC_PRIM_A, CC_SHADE_A, CC_ENV_A, CC_LOD_FRAC, CC_PRIM_LOD_FRAC, CC_K5,
    CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO,
    CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO, CC_ZERO };
const CcInput cc_rgb_d[8] = { CC_COMBINED, CC_TEX0, CC_TEX1, CC_PRIM, CC_SHADE, CC_ENV, CC_ONE, CC_ZERO };
const CcInput cc_alpha_abd[8] = { CC_COMBINED_A, CC_TEX0_A, CC_TEX1_A, CC_PRIM_A, CC_SHADE_A, CC_ENV_A, CC_ONE, CC_ZERO };
const CcInput cc_alpha_c[8] = { CC_LOD_FRAC, CC_TEX0_A, CC_TEX1_A, CC_PRIM_A, CC_SHADE_A, CC_ENV_A, CC_PRIM_LOD_FRAC, CC_ZERO };

// In the second combiner cycle the RDP swaps TEXEL0 and TEXEL1.
CcInput swap_tex(CcInput in) {
    switch (in) {
        case CC_TEX0: return CC_TEX1;
        case CC_TEX1: return CC_TEX0;
        case CC_TEX0_A: return CC_TEX1_A;
        case CC_TEX1_A: return CC_TEX0_A;
        default: return in;
    }
}

bool uses(const CombinerCycle& c, CcInput a, CcInput b) {
    for (int i = 0; i < 4; i++) {
        if (c.rgb[i] == a || c.rgb[i] == b || c.alpha[i] == a || c.alpha[i] == b) return true;
    }
    return false;
}

// Published frame (graphics thread -> main thread).
std::mutex published_mutex;
std::shared_ptr<FrameData> published_frame;
// Frames drawn into off-screen color images, waiting for the backend to render them to RDRAM.
std::deque<std::shared_ptr<FrameData>> offscreen_frames;

} // namespace

struct Renderer::Impl {
    uint8_t* rdram;

    // RSP state
    uint32_t segments[16] = {};
    Mat4 modelview_stack[matrix_stack_size];
    int modelview_depth = 0;
    Mat4 projection = identity();
    Mat4 mvp = identity();
    bool mvp_dirty = true;
    Vertex vertices[vertex_buffer_size];
    uint32_t geometry_mode = 0;
    float viewport_scale[3] = { 160, 120, 511 };
    float viewport_trans[3] = { 160, 120, 511 };
    int num_lights = 0;
    Light lights[8] = {};
    float lookat[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
    int16_t fog_mul = 0, fog_offset = 0;
    uint16_t texture_scale_s = 0xFFFF, texture_scale_t = 0xFFFF;
    uint8_t texture_tile = 0;
    bool texture_on = false;
    uint32_t rdphalf_1 = 0, rdphalf_2 = 0;

    // RDP state
    uint32_t other_mode_l = 0, other_mode_h = 0;
    uint32_t combine_w0 = 0, combine_w1 = 0;
    uint8_t prim[4] = { 255, 255, 255, 255 }, env[4] = { 255, 255, 255, 255 };
    uint8_t fog_color[4] = {}, blend_color[4] = {};
    uint8_t prim_lod_frac = 0;
    uint32_t fill_color = 0;
    Tile tiles[8];
    std::map<uint16_t, TmemLoad> tmem_loads;
    uint16_t palette[256] = {};
    uint64_t palette_hash = 0;    // hash of the whole palette, recomputed by every LOADTLUT
    struct BindEntry {
        std::shared_ptr<TexImage> image;
        uint32_t fingerprint;
        int verified_dl; // display list of the last full hash
        int seen_dl;     // display list of the last bind
        bool volatile_src; // its source has been rewritten before: verified every display list
    };
    std::unordered_map<uint64_t, BindEntry> bind_cache; // see get_texture
    uint32_t timg_addr = 0, timg_width = 1;
    uint8_t timg_siz = 0;
    uint32_t cimg_addr = 0, cimg_width = 320, zimg_addr = 0;
    uint32_t cimg_real_width = 320; // as set; cimg_width is clamped to 64..1024 for the screen mapping
    bool cimg_offscreen = false;    // the color image is an off-screen image (render-to-texture mapping)
    int offscreen_state_logs = 0;   // development: viewport/scissor changes while drawing off screen
    uint16_t scissor[4] = { 0, 0, 320, 240 };

    // Output
    std::unordered_map<uint32_t, std::unique_ptr<FrameData>> frames; // keyed by color image address
    FrameData* frame = nullptr;
    bool state_dirty = true;
    // Rectangles: consecutive ones with no state change in between extend one draw (Pokemon Snap's
    // camera check draws ~1300 sprite rectangles per frame, mostly repeating the same texture and
    // state). Set by every state change; rect() keeps what the last rectangle's draw was made with.
    bool rect_state_changed = true;
    // Rectangles depth-tested against the primitive depth (G_SETPRIMDEPTH with the Z source on
    // primitive, other mode L bit 2): Pokemon Snap's particles (smoke, sparks, Haunter's clouds) are
    // texture rectangles placed at their projected depth. That depth (screen z * 32) is turned back into
    // the distance w the 3D geometry is depth-tested by (see rect_primitive_w); 0 = no depth test.
    uint16_t prim_depth = 0;
    float rect_prim_w = 0.0f;
    void mark_state_changed() {
        state_dirty = true;
        rect_state_changed = true;
    }
    std::unordered_map<uint64_t, std::shared_ptr<TexImage>> texture_cache;
    std::unordered_map<uint32_t, std::shared_ptr<FrameData>> last_published; // per framebuffer

    // True if the frame redraws the whole screen: either one of its first draws is an opaque
    // full-screen fill, or its screen-space rectangles (texrects/fillrects, w == 1) together span the
    // full framebuffer (e.g. a picture drawn as alpha-blended tiles).
    static bool redraws_whole_screen(const FrameData& f) {
        auto covers = [](float min_x, float max_x, float min_y, float max_y) {
            // Rotated top-screen space: pos[0] spans the 240-pixel axis (-1..1), pos[1] the 4:3 area (-0.8..0.8).
            return min_x <= -0.99f && max_x >= 0.99f && min_y <= -0.79f && max_y >= 0.79f;
        };
        float all_min_x = 1e9f, all_max_x = -1e9f, all_min_y = 1e9f, all_max_y = -1e9f;
        for (size_t i = 0; i < f.draws.size(); i++) {
            const DrawCmd& d = f.draws[i];
            // A z-buffer clear at the start: a fresh 3D scene, drawn whole (Pokemon Snap's levels
            // cover the screen with a sky dome and terrain, never with a fill).
            if (d.depth_clear) {
                if (i < 4) return true;
                continue;
            }
            float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
            bool screen_space = true;
            for (uint32_t v = d.first_vertex; v < d.first_vertex + d.vertex_count; v++) {
                const GpuVertex& g = f.vertices[v];
                if (g.pos[3] != 1.0f) { screen_space = false; break; }
                min_x = std::min(min_x, g.pos[0]); max_x = std::max(max_x, g.pos[0]);
                min_y = std::min(min_y, g.pos[1]); max_y = std::max(max_y, g.pos[1]);
            }
            if (!screen_space || d.vertex_count == 0) continue;
            if (i < 4 && d.blend == BlendMode::Opaque && covers(min_x, max_x, min_y, max_y)) return true;
            all_min_x = std::min(all_min_x, min_x); all_max_x = std::max(all_max_x, max_x);
            all_min_y = std::min(all_min_y, min_y); all_max_y = std::max(all_max_y, max_y);
        }
        return covers(all_min_x, all_max_x, all_min_y, all_max_y);
    }

    // N64 framebuffers keep their contents, so a frame that doesn't redraw the whole screen is drawn
    // on top of the previous frame from the same framebuffer. Only one level deep (onto the previous
    // frame as drawn, never onto a composite) so draw counts can't snowball.
    std::shared_ptr<FrameData> composite_with_previous(uint32_t addr, std::shared_ptr<FrameData> cur) {
        if (redraws_whole_screen(*cur)) return cur;
        auto prev_it = last_published.find(addr);
        if (prev_it == last_published.end()) return cur;
        const FrameData& prev = *prev_it->second;
        if (prev.vertices.size() + cur->vertices.size() > max_frame_vertices / 4) return cur;
        auto merged = std::make_shared<FrameData>();
        merged->fb_address = addr;
        merged->vertices.reserve(prev.vertices.size() + cur->vertices.size());
        merged->vertices = prev.vertices;
        merged->vertices.insert(merged->vertices.end(), cur->vertices.begin(), cur->vertices.end());
        merged->draws = prev.draws;
        uint32_t offset = (uint32_t)prev.vertices.size();
        for (DrawCmd d : cur->draws) {
            d.first_vertex += offset;
            merged->draws.push_back(d);
        }
        return merged;
    }

    // One-time survey written to stderr (runtime.log)
    int dl_count = 0;
    uint32_t opcode_counts[256] = {};
    std::unordered_set<uint64_t> logged_combiners;
    std::unordered_set<uint32_t> logged_unknown;
    int runaway_logs = 0;
    int logged_dark_ci = 0;
    uint32_t last_tlut_base = 0, last_tlut_count = 0, last_tlut_src = 0;
    bool dumped_big_frame = false;

    static bool is_3d_draw(const FrameData& f, const DrawCmd& d) {
        return d.vertex_count > 0 && f.vertices[d.first_vertex].pos[3] != 1.0f;
    }

    // Development aid: one-line summary per 3D draw of the first frame with real 3D content.
    static void dump_frame(const FrameData& f) {
        fprintf(stderr, "[dump] fb=0x%06X draws=%u verts=%u\n", (unsigned)f.fb_address, (unsigned)f.draws.size(), (unsigned)f.vertices.size());
        int n = 0;
        for (const DrawCmd& d : f.draws) {
            if (!is_3d_draw(f, d)) continue;
            if (n++ >= 60) break;
            const GpuVertex& v = f.vertices[d.first_vertex];
            fprintf(stderr, "[dump] %3d n=%-4lu %s%s%s geo=%06lX cc=%d,%d,%d,%d/%d,%d,%d,%d cc2=%d,%d,%d,%d/%d,%d,%d,%d tex=%dx%d prim=%02X%02X%02X%02X env=%02X%02X%02X%02X bl=%d at=%d zt=%d zw=%d zc=%d v0=(%.2f %.2f %.3f %.2f) c=%02X%02X%02X%02X\n",
                n, d.vertex_count, d.copy_mode ? "C" : "", d.fill_mode ? "F" : "", d.two_cycle ? "2" : "1", d.geometry_mode,
                d.cycles[0].rgb[0], d.cycles[0].rgb[1], d.cycles[0].rgb[2], d.cycles[0].rgb[3],
                d.cycles[0].alpha[0], d.cycles[0].alpha[1], d.cycles[0].alpha[2], d.cycles[0].alpha[3],
                d.cycles[1].rgb[0], d.cycles[1].rgb[1], d.cycles[1].rgb[2], d.cycles[1].rgb[3],
                d.cycles[1].alpha[0], d.cycles[1].alpha[1], d.cycles[1].alpha[2], d.cycles[1].alpha[3],
                d.tex[0].image ? d.tex[0].image->width : 0, d.tex[0].image ? d.tex[0].image->height : 0,
                d.prim[0], d.prim[1], d.prim[2], d.prim[3], d.env[0], d.env[1], d.env[2], d.env[3],
                (int)d.blend, d.alpha_test, d.depth_test, d.depth_write, d.depth_clear,
                v.pos[0], v.pos[1], v.pos[2], v.pos[3], v.color[0], v.color[1], v.color[2], v.color[3]);
        }
    }

    explicit Impl(uint8_t* r) : rdram(r) {
        modelview_stack[0] = identity();
    }

    // --- RDRAM access ---

    uint32_t resolve(uint32_t addr) const {
        return (segments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF)) & 0x00FFFFFF;
    }
    bool in_range(uint32_t phys, uint32_t size) const {
        return phys < rdram_limit && phys + size <= rdram_limit;
    }
    uint32_t read_u32(uint32_t phys) const {
        return in_range(phys, 4) ? *reinterpret_cast<const uint32_t*>(rdram + (phys & ~3u)) : 0;
    }
    uint16_t read_u16(uint32_t phys) const {
        return in_range(phys, 2) ? *reinterpret_cast<const uint16_t*>(rdram + ((phys & ~1u) ^ 2)) : 0;
    }
    uint8_t read_u8(uint32_t phys) const {
        return phys < rdram_limit ? rdram[phys ^ 3] : 0;
    }

    // --- Frames ---

    void select_frame(uint32_t addr) {
        auto& slot = frames[addr];
        if (!slot) {
            slot = std::make_unique<FrameData>();
            slot->fb_address = addr;
        }
        slot->fb_width = cimg_real_width;
        frame = slot.get();
        mark_state_changed();
        sync_candidates_valid = false;
    }

    // Render-to-RAM. Color images the VI has shown are framebuffers; draws into any other one (at
    // least 64 pixels wide) are an off-screen image the game reads back, like Pokemon Snap's photos,
    // which it renders again whenever it shows them. After each display list such frames go to the
    // backend, which renders them and writes the pixels to RDRAM.
    std::unordered_set<uint32_t> vi_origins; // every VI_ORIGIN seen (a little past a framebuffer's start)
    std::unordered_set<uint32_t> depth_images; // every z-buffer address set (never an off-screen image)

    bool shown_by_vi(uint32_t addr) const {
        for (uint32_t o : vi_origins) {
            if (o >= addr && o - addr < 0x2000) return true;
        }
        return false;
    }

    bool is_offscreen(uint32_t addr, const FrameData& f) const {
        return vi_origins.size() >= 2 // until both framebuffers have been shown, nothing is
            && !depth_images.count(addr) && f.fb_width >= 64 && !shown_by_vi(addr) && !f.draws.empty();
    }

    // The rows its draws' scissors reach (images can be taller than 3/4 of their width).
    // Cached per frame: only draws added since the last call are scanned (Pokemon Snap's camera
    // check binds ~1300 textures per frame, each checked against every image, see sync_offscreen_reads).
    static uint32_t image_bytes(FrameData& f) {
        if (f.rows_scanned > f.draws.size()) { // the frame was cleared and reused
            f.rows_scanned = 0;
            f.rows = 1;
        }
        for (size_t i = f.rows_scanned; i < f.draws.size(); i++) {
            if (f.draws[i].offscreen_map) f.rows = std::max<uint32_t>(f.rows, f.draws[i].scissor[3]);
        }
        f.rows_scanned = f.draws.size();
        return f.fb_width * f.rows * 2;
    }

    // Sent to the backend, not yet drawn.
    std::vector<std::shared_ptr<FrameData>> offscreen_inflight;
    // Off-screen images the backend keeps as render targets (address -> width): texture loads from
    // them bind the target (render-to-texture, see get_texture).
    // An alias is an image the game only copied out of another one (Pokemon Snap cuts its thumbnail
    // sheet into strips that way): it has no target of its own and reads from its source's at
    // (ax, ay).
    struct OffImage {
        uint32_t width, height;
        uint32_t alias_of = 0;
        float ax = 0, ay = 0;
    };
    std::unordered_map<uint32_t, OffImage> offscreen_images;

    void send_offscreen(std::unordered_map<uint32_t, std::unique_ptr<FrameData>>::iterator it) {
        std::shared_ptr<FrameData> done{ it->second.release() };
        if (frame == done.get()) frame = nullptr;
        frames.erase(it);
        sync_candidates_valid = false;
        done->rdram = rdram;
        static int logged;
        if (logged < 24) {
            logged++;
            fprintf(stderr, "[gfx] off-screen image 0x%06X width %u: %u draws (dl %d)\n", (unsigned)done->fb_address,
                (unsigned)done->fb_width, (unsigned)done->draws.size(), dl_count);
        }
        uint32_t height = 1;
        const TexImage* copy_source = nullptr;
        bool only_copies = true;
        for (const DrawCmd& d : done->draws) {
            if (d.offscreen_map) height = std::max<uint32_t>(height, d.scissor[3]);
            if (d.depth_clear) continue;
            if (d.copy_mode && d.tex[0].image && d.tex[0].image->rt_address) copy_source = d.tex[0].image.get();
            else only_copies = false;
        }
        OffImage image{ done->fb_width, std::min<uint32_t>(height, 256) };
        if (false && only_copies && copy_source) { // aliases only with render-to-texture
            auto info = rt_info.find(copy_source);
            if (info != rt_info.end()) {
                image.alias_of = copy_source->rt_address;
                image.ax = info->second.x0;
                image.ay = info->second.y0;
                offscreen_images[done->fb_address] = image;
                done->written = true;
                return; // nothing to draw: a view of its source
            }
        }
        offscreen_images[done->fb_address] = image;
        offscreen_inflight.push_back(done);
        std::lock_guard lock{ published_mutex };
        offscreen_frames.push_back(std::move(done));
    }

    // At the end of a display list every off-screen image it drew is sent, and the task only completes
    // once they're all back in RDRAM: the game's CPU reads them as soon as the task is done (Pokemon
    // Snap averages the 2x photo re-render into its thumbnail bitmaps, which a GPU fill cleared first).
    void finish_offscreen_frames() {
        finish_offscreen_sends();
        sync_offscreen_reads(0, 0x800000);
        static int zr_dl_logs;
        if (depth_raster_tris && zr_dl_logs < 16) { // development: what the photo scoring will read
            zr_dl_logs++;
            uint32_t base = 0x2B9E00;
            fprintf(stderr, "[zr] end of dl %d: %u tris %u pixels; z buffers (non-FFFC / zero):", dl_count,
                (unsigned)depth_raster_tris, (unsigned)depth_raster_pixels);
            for (int b = 0; b < 4; b++) {
                unsigned set = 0, zero = 0;
                for (uint32_t i = 0; i < 64 * 48; i++) {
                    uint16_t v = *reinterpret_cast<uint16_t*>(rdram + ((base + (b * 3072 + i) * 2) ^ 2));
                    set += v != 0xFFFC;
                    zero += v == 0;
                }
                fprintf(stderr, " %u/%u", set, zero);
            }
            fprintf(stderr, "\n");
        }
        depth_raster_tris = depth_raster_pixels = 0;
    }

    void finish_offscreen_sends() {
        for (auto it = frames.begin(); it != frames.end();) {
            FrameData& f = *it->second;
            if (depth_images.count(it->first) && frame != &f) {
                sync_candidates_valid = false;
                f.vertices.clear(); // draws into a z-buffer show nowhere; don't let them pile up
                f.draws.clear();
            }
            if (is_offscreen(it->first, f)) {
                auto next = std::next(it);
                send_offscreen(it);
                it = next;
            } else {
                ++it;
            }
        }
    }

    // Before reading RDRAM [start, start + len) as a texture: an off-screen image being built there is
    // sent to the backend first, so it's drawn before what reads it (Pokemon Snap's photo thumbnails
    // are rendered and copied in one display list).
    // Off-screen images other than the current one, as RDRAM ranges: rebuilt only when the current
    // image, the set of frames or what counts as off-screen changes (draws only go to the current
    // image). Pokemon Snap's camera check binds ~1300 textures per frame and each bind checks them.
    std::vector<std::pair<uint32_t, uint32_t>> sync_candidates;
    bool sync_candidates_valid = false;

    void sync_offscreen_reads(uint32_t start, uint32_t len) {
        if (!sync_candidates_valid) {
            sync_candidates.clear();
            for (auto& [addr, f] : frames) {
                if (f.get() != frame && is_offscreen(addr, *f)) sync_candidates.emplace_back(addr, addr + image_bytes(*f));
            }
            sync_candidates_valid = true;
        }
        bool any = false;
        for (const auto& [lo, hi] : sync_candidates) any |= start < hi && lo < start + len;
        if (!any && offscreen_inflight.empty()) return;
        sync_candidates_valid = false;
        auto overlaps = [&](FrameData& f) {
            return start < f.fb_address + image_bytes(f) && f.fb_address < start + len;
        };
        for (auto it = frames.begin(); it != frames.end();) {
            FrameData& f = *it->second;
            if (&f != frame && is_offscreen(it->first, f) && overlaps(f)) {
                auto next = std::next(it);
                send_offscreen(it);
                it = next;
            } else {
                ++it;
            }
        }
        for (const auto& f : offscreen_inflight) {
            if (f->written || !overlaps(*f)) continue;
            for (int i = 0; i < 1000 && !f->written; i++) std::this_thread::sleep_for(std::chrono::milliseconds(1)); // at most ~1 s
            if (!f->written) fprintf(stderr, "[gfx] off-screen image 0x%06X not written in time\n", (unsigned)f->fb_address);
        }
        offscreen_inflight.erase(std::remove_if(offscreen_inflight.begin(), offscreen_inflight.end(),
            [](const std::shared_ptr<FrameData>& f) { return f->written.load(); }), offscreen_inflight.end());
    }

    // See n64_isolate_lo. The zoom is fixed from the first isolated frame (its bounding box filling
    // 80% of the screen height), so the character's own movement stays visible.
    void isolate(FrameData& f) {
        static bool fixed;
        static float c0, c1, zoom;
        float min0 = 1e9f, max0 = -1e9f, min1 = 1e9f, max1 = -1e9f;
        for (DrawCmd& d : f.draws) {
            const TexImage* img = d.tex[0].image.get();
            bool keep = img && img->src >= n64_isolate_lo && img->src < n64_isolate_hi;
            if (!keep) {
                if (!d.depth_clear) d.vertex_count = 0;
                continue;
            }
            for (uint32_t i = 0; i < d.vertex_count; i++) {
                const GpuVertex& v = f.vertices[d.first_vertex + i];
                if (v.pos[3] <= 0) continue;
                float a = v.pos[0] / v.pos[3], b = v.pos[1] / v.pos[3];
                min0 = std::min(min0, a); max0 = std::max(max0, a); min1 = std::min(min1, b); max1 = std::max(max1, b);
            }
        }
        if (min0 > max0) return;
        if (!fixed) {
            fixed = true;
            c0 = (min0 + max0) / 2;
            c1 = (min1 + max1) / 2;
            zoom = 1.6f / std::max(max0 - min0, 0.01f); // portrait target: pos[0] is the screen's vertical axis
            zoom = std::min(zoom, 1.8f / std::max(max1 - min1, 0.01f)); // and the whole width fits
            if (max0 - min0 > 1.8f) { // already fills the screen (a background): as it is
                c0 = c1 = 0.0f;
                zoom = 1.0f;
            }
            fprintf(stderr, "[isolate] center %.3f %.3f zoom %.2f\n", c0, c1, zoom);
        }
        for (DrawCmd& d : f.draws) {
            if (!d.vertex_count || d.depth_clear) continue;
            for (uint32_t i = 0; i < d.vertex_count; i++) {
                GpuVertex& v = f.vertices[d.first_vertex + i];
                v.pos[0] = (v.pos[0] / v.pos[3] - c0) * zoom * v.pos[3];
                v.pos[1] = (v.pos[1] / v.pos[3] - c1) * zoom * v.pos[3];
            }
        }
    }

    void publish(uint32_t vi_origin) {
        vi_origins.insert(vi_origin);
        sync_candidates_valid = false;
        // VI_ORIGIN usually points a little past the start of the framebuffer.
        for (auto it = frames.begin(); it != frames.end(); ++it) {
            uint32_t addr = it->first;
            if (vi_origin >= addr && vi_origin - addr < 0x2000 && !it->second->draws.empty()) {
                std::shared_ptr<FrameData> done{ it->second.release() };
                if (frame == done.get()) frame = nullptr;
                frames.erase(it);
                sync_candidates_valid = false;
                if (!dumped_big_frame) {
                    int draws_3d = 0;
                    for (const DrawCmd& d : done->draws) draws_3d += is_3d_draw(*done, d);
                    if (draws_3d >= 20) {
                        dumped_big_frame = true;
                        dump_frame(*done);
                    }
                }
                if (n64_isolate_hi) isolate(*done);
                if (snap_boot_skip) {
                    for (DrawCmd& d : done->draws) d.vertex_count = 0; // black while the boot narration is skipped
                }
                // Pokemon Snap paints a red dot (5x5, corners cut, at 157,117) into the finished
                // framebuffer with the CPU when a Pokemon is in focus (PokemonDetector_PostProcessImage).
                // Framebuffers are only drawn on the GPU here, so the dot is read from RDRAM, cleared
                // (stale pixels would light it later) and drawn by the front end (n64_top_red_dot).
                bool red_dot = false;
                if (done->fb_width == 320 && in_range(addr, 320 * 240 * 2)) {
                    auto px = [&](int x, int y) -> uint16_t& {
                        return *reinterpret_cast<uint16_t*>(rdram + ((addr + (uint32_t)(y * 320 + x) * 2) ^ 2));
                    };
                    red_dot = px(159, 119) == 0xF801 && px(158, 117) == 0xF801;
                    if (red_dot) {
                        for (int y = 117; y < 122; y++)
                            for (int x = 157; x < 162; x++) px(x, y) = 0;
                    }
                }
                std::shared_ptr<FrameData> raw = done;
                if (!n64_isolate_hi) done = composite_with_previous(addr, done); // uses the previous raw frame
                done->red_dot = red_dot;
                last_published[addr] = raw;
                std::lock_guard lock{ published_mutex };
                published_frame = std::move(done);
                return;
            }
        }
    }

    // --- Matrices & vertices ---

    Mat4 read_matrix(uint32_t phys) const {
        Mat4 r;
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                int16_t whole = (int16_t)read_u16(phys + (i * 4 + j) * 2);
                uint16_t frac = read_u16(phys + 32 + (i * 4 + j) * 2);
                r.m[i][j] = whole + frac / 65536.0f;
            }
        }
        return r;
    }

    void update_mvp() {
        if (mvp_dirty) {
            mvp = mul(modelview_stack[modelview_depth], projection);
            mvp_dirty = false;
        }
    }

    // Transforms a vector by the upper 3x3 of the modelview transposed (model space light direction).
    void to_model_space(const float in[3], float out[3]) {
        const Mat4& mv = modelview_stack[modelview_depth];
        for (int i = 0; i < 3; i++) {
            out[i] = in[0] * mv.m[i][0] + in[1] * mv.m[i][1] + in[2] * mv.m[i][2];
        }
        float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
        if (len > 0.0f) {
            out[0] /= len; out[1] /= len; out[2] /= len;
        }
    }

    void load_vertices(uint32_t phys, int count, int dest) {
        Phase phase(PHASE_VTX);
        update_mvp();
        float light_dirs[8][3];
        float lookat_dirs[2][3];
        bool lighting = geometry_mode & G_LIGHTING;
        if (lighting) {
            for (int l = 0; l < num_lights; l++) to_model_space(lights[l].dir, light_dirs[l]);
            to_model_space(lookat[0], lookat_dirs[0]);
            to_model_space(lookat[1], lookat_dirs[1]);
        }
        float fb_wf = (float)cimg_width;
        float fb_hf = fb_wf * 0.75f;
        float kx = (top_h * 4.0f / 3.0f / top_w) / (fb_wf * 0.5f); // pillarbox: 320 px -> 0.8 of the width
        float inv_half_h = 1.0f / (fb_hf * 0.5f);
        float s_scale = texture_scale_s / 65536.0f / 32.0f, t_scale = texture_scale_t / 65536.0f / 32.0f;
        bool perspective = mvp.m[0][3] != 0.0f || mvp.m[1][3] != 0.0f || mvp.m[2][3] != 0.0f;

        for (int i = 0; i < count && dest + i < vertex_buffer_size; i++) {
            uint32_t p = phys + i * 16;
            // One vertex is 16 bytes: x y z flag (s16 each), s t (s16), r g b a (or normal + alpha).
            uint32_t vw[4] = {};
            if ((p & 3) == 0 && in_range(p, 16)) {
                memcpy(vw, rdram + p, 16); // RDRAM words are in host order
            } else {
                for (int k = 0; k < 4; k++) vw[k] = (uint32_t)read_u16(p + k * 4) << 16 | read_u16(p + k * 4 + 2);
            }
            float x = (int16_t)(vw[0] >> 16);
            float y = (int16_t)vw[0];
            float z = (int16_t)(vw[1] >> 16);
            int16_t s = (int16_t)(vw[2] >> 16);
            int16_t t = (int16_t)vw[2];
            uint8_t c0 = vw[3] >> 24, c1 = vw[3] >> 16, c2 = vw[3] >> 8, alpha = vw[3];

            Vertex& v = vertices[dest + i];
            float cx = x * mvp.m[0][0] + y * mvp.m[1][0] + z * mvp.m[2][0] + mvp.m[3][0];
            float cy = x * mvp.m[0][1] + y * mvp.m[1][1] + z * mvp.m[2][1] + mvp.m[3][1];
            float cz = x * mvp.m[0][2] + y * mvp.m[1][2] + z * mvp.m[2][2] + mvp.m[3][2];
            float cw = x * mvp.m[0][3] + y * mvp.m[1][3] + z * mvp.m[2][3] + mvp.m[3][3];

            // Viewport: screen = ndc * scale + trans (y down), then N64 screen -> 3DS clip space.
            if (cimg_offscreen) { // 1:1 into the off-screen target's 512x256 area
                v.x = (cx * viewport_scale[0] + cw * viewport_trans[0]) / 256.0f - cw;
                v.y = cw - (cw * viewport_trans[1] - cy * viewport_scale[1]) / 128.0f;
            } else {
                v.x = (cx * viewport_scale[0] + cw * (viewport_trans[0] - fb_wf * 0.5f)) * kx;
                v.y = (cy * viewport_scale[1] + cw * (fb_hf * 0.5f - viewport_trans[1])) * inv_half_h;
            }
            // F3DEX2_NoN clips neither at the near nor at the far plane, only at the camera (w = 0).
            // The PICA200 clips at -w <= z <= 0, so perspective vertices get z = -w/2: those planes
            // become w >= 0, and with the w-buffer (c3d_backend.cpp) depth is the distance w itself.
            // (Clipping at the N64 near plane cut away Pokemon Snap's ground near the camera.)
            // Orthographic vertices (w = 1) keep the N64 depth, mapped to [0, -w].
            v.z = perspective ? -0.5f * cw : -(cz + cw) * 0.5f;
            v.w = cw;
            // (The viewport's z scale and translation aren't in quarter units like x and y.)
            v.screen_z = cw > 0.0f ? cz / cw * viewport_scale[2] * 4.0f + viewport_trans[2] * 4.0f : 65535.0f;

            if (lighting) {
                float n[3] = { (float)(int8_t)c0, (float)(int8_t)c1, (float)(int8_t)c2 };
                float col[3] = { lights[num_lights].col[0], lights[num_lights].col[1], lights[num_lights].col[2] };
                for (int l = 0; l < num_lights; l++) {
                    float d = (n[0] * light_dirs[l][0] + n[1] * light_dirs[l][1] + n[2] * light_dirs[l][2]) / 127.0f;
                    if (d > 0.0f) {
                        col[0] += d * lights[l].col[0];
                        col[1] += d * lights[l].col[1];
                        col[2] += d * lights[l].col[2];
                    }
                }
                for (int k = 0; k < 3; k++) v.color[k] = (uint8_t)std::min(255.0f, col[k]);

                if (geometry_mode & G_TEXTURE_GEN) {
                    float dx = (n[0] * lookat_dirs[0][0] + n[1] * lookat_dirs[0][1] + n[2] * lookat_dirs[0][2]) / 127.0f;
                    float dy = (n[0] * lookat_dirs[1][0] + n[1] * lookat_dirs[1][1] + n[2] * lookat_dirs[1][2]) / 127.0f;
                    if (geometry_mode & G_TEXTURE_GEN_LINEAR) {
                        dx = std::acos(-std::clamp(dx, -1.0f, 1.0f)) / 3.14159265f * 2.0f - 1.0f;
                        dy = std::acos(-std::clamp(dy, -1.0f, 1.0f)) / 3.14159265f * 2.0f - 1.0f;
                    }
                    s = (int16_t)std::clamp((dx + 1.0f) * 16384.0f, -32768.0f, 32767.0f);
                    t = (int16_t)std::clamp((dy + 1.0f) * 16384.0f, -32768.0f, 32767.0f);
                }
            }
            else {
                v.color[0] = c0; v.color[1] = c1; v.color[2] = c2;
            }
            v.color[3] = alpha;

            if (geometry_mode & G_FOG) {
                float w = std::fabs(cw) < 0.001f ? 0.001f : cw;
                float f = cz / w * fog_mul + fog_offset;
                v.color[3] = (uint8_t)std::clamp(f, 0.0f, 255.0f);
            }

            v.u = s * s_scale;
            v.v = t * t_scale;
        }
    }

    // --- Textures ---

    static uint32_t bytes_per_texel_x2(uint8_t siz) { return siz == 0 ? 1 : siz == 1 ? 2 : siz == 2 ? 4 : 8; }

    // A load of the same source with the same layout changes nothing (sprites reload their texture
    // before every rectangle).
    void set_tmem_load(uint16_t tmem, const TmemLoad& l) {
        auto it = tmem_loads.find(tmem);
        if (it != tmem_loads.end() && it->second.src == l.src && it->second.src_stride == l.src_stride &&
            it->second.tmem_line == l.tmem_line && it->second.odd_rows_swapped == l.odd_rows_swapped) return;
        tmem_loads[tmem] = l;
        mark_state_changed();
    }

    const TmemLoad* find_load(uint16_t tmem, uint16_t& load_tmem) const {
        auto it = tmem_loads.upper_bound(tmem);
        if (it == tmem_loads.begin()) return nullptr;
        --it;
        load_tmem = it->first;
        return &it->second;
    }

    // xr: XOR applied to the byte offset within the row (odd rows of a dxt-0 LOADBLOCK, see TmemLoad).
    uint32_t decode_texel(const Tile& tile, uint32_t row_addr, uint32_t x, uint8_t tlut_type, uint32_t xr = 0) const {
        switch (tile.siz) {
            case G_IM_SIZ_4b: {
                uint8_t b = read_u8(row_addr + ((x / 2) ^ xr));
                uint8_t n = (x & 1) ? (b & 0xF) : (b >> 4);
                if (tile.fmt == G_IM_FMT_CI) {
                    uint16_t c = palette[(tile.palette * 16 + n) & 0xFF];
                    return tlut_type == G_TT_IA16 ? ia16(c) : rgba5551(c);
                }
                if (tile.fmt == G_IM_FMT_IA) {
                    uint8_t i = (uint8_t)(((n >> 1) & 7) * 255 / 7);
                    return rgba(i, i, i, (n & 1) ? 255 : 0);
                }
                uint8_t i = n * 17;
                return rgba(i, i, i, i);
            }
            case G_IM_SIZ_8b: {
                uint8_t b = read_u8(row_addr + (x ^ xr));
                if (tile.fmt == G_IM_FMT_CI) {
                    uint16_t c = palette[b];
                    return tlut_type == G_TT_IA16 ? ia16(c) : rgba5551(c);
                }
                if (tile.fmt == G_IM_FMT_IA) {
                    uint8_t i = (b >> 4) * 17;
                    return rgba(i, i, i, (b & 0xF) * 17);
                }
                return rgba(b, b, b, b);
            }
            case G_IM_SIZ_16b: {
                uint32_t a = row_addr + ((x * 2) ^ xr);
                uint16_t c = (uint16_t)((read_u8(a) << 8) | read_u8(a + 1));
                if (tile.fmt == G_IM_FMT_IA) return ia16(c);
                return rgba5551(c);
            }
            default: {
                uint32_t a = row_addr + ((x * 4) ^ xr);
                return rgba(read_u8(a), read_u8(a + 1), read_u8(a + 2), read_u8(a + 3));
            }
        }
    }

    // Hash of a texture's source bytes. Hashing every bind's bytes (~150 KB per frame in a busy area)
    // was the largest part of the interpretation time, so each source (address and layout) keeps its
    // last hash with a fingerprint of 16 sampled words: while the fingerprint matches, the stored
    // hash is reused, and every 8th display list the bytes are hashed in full anyway, so a texture the
    // game rewrites only partly is stale for at most 8 frames. A source whose content has changed once
    // is volatile and hashed in full every display list: Pokemon Snap prints dialogue a character at
    // a time into a text bitmap, which the fingerprint rarely sees, so text came out in bursts.
    struct SourceInfo {
        uint64_t content;
        uint32_t fingerprint;
        int verified_dl;
        bool volatile_src;
    };
    bool last_source_volatile = false; // of the last source_content_hash call
    std::unordered_map<uint64_t, SourceInfo> source_cache;

    // Palette (CI) textures by their indices alone: some scenes rewrite a palette every frame (the
    // refractor cutscene's light plays over Mega Man through ~100 small CI4 textures), which made
    // every one of them a new texture each frame, decoded texel by texel. With the indices kept, a
    // new palette costs one table lookup per texel.
    struct IndexImage {
        std::vector<uint8_t> indices; // Morton-tiled like TexImage::texels
        int used_dl;
    };
    std::unordered_map<uint64_t, IndexImage> index_cache;
    size_t index_bytes = 0;
    size_t texture_bytes = 0; // texels of texture_cache's images

    static size_t image_bytes(const TexImage& img) { return img.texels.size() * sizeof(uint32_t); }

    // Keeps texture_cache within its count and memory limits: drops what no display list used
    // recently, then what this one hasn't used, then everything. The bind memo holds references to the
    // images too, so it's cleared whenever any go (it refills in one frame).
    void trim_texture_cache() {
        if (texture_cache.size() <= texture_cache_limit && texture_bytes <= texture_cache_budget) return;
        for (int age : { 4, 1 }) {
            for (auto e = texture_cache.begin(); e != texture_cache.end();) {
                if ((int)e->second->last_used + age < dl_count) {
                    texture_bytes -= image_bytes(*e->second);
                    e = texture_cache.erase(e);
                } else {
                    ++e;
                }
            }
            if (texture_cache.size() <= texture_cache_limit && texture_bytes <= texture_cache_budget * 3 / 4) break;
        }
        if (texture_cache.size() > texture_cache_limit || texture_bytes > texture_cache_budget) {
            texture_cache.clear();
            texture_bytes = 0;
        }
        bind_cache.clear();
    }

    void trim_index_cache() {
        if (index_bytes <= index_cache_budget) return;
        for (auto e = index_cache.begin(); e != index_cache.end();) {
            if (e->second.used_dl + 2 < dl_count) {
                index_bytes -= e->second.indices.size();
                e = index_cache.erase(e);
            } else {
                ++e;
            }
        }
        if (index_bytes > index_cache_budget) {
            index_cache.clear();
            index_bytes = 0;
        }
    }

    uint32_t source_fingerprint(uint32_t start, uint32_t stride, uint32_t row_bytes, uint32_t h) const {
        uint32_t f = 0x811C9DC5u;
        for (uint32_t i = 0; i < 16; i++) {
            uint32_t row = h > 1 ? (h - 1) * (i / 2) / 7 : 0;
            uint32_t off = (i & 1) && row_bytes >= 4 ? ((row_bytes - 4) & ~3u) : 0;
            uint32_t a = (start + row * stride + off) & ~3u;
            uint32_t word = in_range(a, 4) ? *reinterpret_cast<const uint32_t*>(rdram + a) : 0;
            f = (f ^ word) * 0x01000193u;
        }
        return f;
    }

    uint64_t source_content_hash(uint32_t start, uint32_t stride, uint32_t row_bytes, uint32_t w, uint32_t h, uint32_t fmt_siz) {
        uint32_t layout[5] = { start, stride, row_bytes, w << 16 | h, fmt_siz };
        uint64_t id = fnv1a(0xCBF29CE484222325ull, layout, sizeof(layout));
        uint32_t fingerprint = source_fingerprint(start, stride, row_bytes, h);
        auto it = source_cache.find(id);
        bool was_volatile = it != source_cache.end() && it->second.volatile_src;
        if (!n64_tex_nocache && it != source_cache.end() && !was_volatile && it->second.fingerprint == fingerprint &&
            dl_count - it->second.verified_dl < 8) {
            last_source_volatile = false;
            return it->second.content;
        }
        uint64_t content = 0x9E3779B97F4A7C15ull;
        if (stride == row_bytes && in_range(start, row_bytes * h)) {
            content = hash_bytes(content, rdram + start, row_bytes * h); // contiguous: one pass
            tex_hashed_bytes += row_bytes * h;
        } else {
            for (uint32_t y = 0; y < h; y++) {
                uint32_t row = start + y * stride;
                if (in_range(row, row_bytes)) content = hash_bytes(content, rdram + row, row_bytes); // word-swapped, still unique
                tex_hashed_bytes += row_bytes;
            }
        }
        bool changed = it != source_cache.end() && it->second.content != content;
        last_source_volatile = was_volatile || changed;
        if (source_cache.size() > 8192) source_cache.clear();
        source_cache[id] = { content, fingerprint, dl_count, last_source_volatile };
        return content;
    }

    // A texture whose texels start inside an off-screen image: that image's render target. The
    // image pixel at the texture's start is kept (rt_x0/rt_y0) for to_gpu's coordinate mapping.
    struct RtInfo { uint32_t image_width; float x0, y0; };
    std::unordered_map<const TexImage*, RtInfo> rt_info;
    std::unordered_map<uint64_t, std::shared_ptr<TexImage>> rt_images;

    const RtInfo* rt_of(const TexBinding& b) const {
        if (!b.image || !b.image->rt_address) return nullptr;
        auto it = rt_info.find(b.image.get());
        return it == rt_info.end() ? nullptr : &it->second;
    }

    std::shared_ptr<TexImage> offscreen_texture(uint32_t start) {
        for (const auto& [addr, image] : offscreen_images) {
            if (start < addr || start >= addr + image.width * image.height * 2) continue;
            uint32_t pixel = (start - addr) / 2;
            float x = (float)(pixel % image.width), y = (float)(pixel / image.width);
            uint32_t target = addr;
            if (image.alias_of) {
                target = image.alias_of;
                x += image.ax;
                y += image.ay;
            }
            uint64_t key = (uint64_t)addr << 32 | start;
            auto& img = rt_images[key];
            if (!img) {
                img = std::make_shared<TexImage>();
                img->key = 0xE000000000000000ull | key;
                img->width = 256;
                img->height = 512;
                img->rt_address = target;
                img->src = start;
                img->fmt = G_IM_FMT_RGBA;
                img->siz = G_IM_SIZ_16b;
                rt_info[img.get()] = RtInfo{ image.width, x, y };
            }
            return img;
        }
        return nullptr;
    }

    // A clamped tile larger than its mask that get_texture decodes at the tile's size (wrapping done
    // on the CPU), so it can be clamped at the tile's edge.
    static bool tile_unrolled(uint32_t cm, uint32_t mask, uint32_t extent) {
        return mask && (cm & G_TX_CLAMP) && extent > (1u << mask) + 1 && extent <= 256;
    }

    // Decodes the texture a render tile points at; returns null if nothing was loaded there.
    std::shared_ptr<TexImage> get_texture(const Tile& tile, uint32_t& out_w, uint32_t& out_h) {
        uint16_t load_tmem;
        const TmemLoad* load = find_load(tile.tmem, load_tmem);
        if (!load) return nullptr;

        uint32_t w = tile.masks ? (1u << tile.masks) : ((tile.lrs - tile.uls) >> 2) + 1;
        uint32_t h = tile.maskt ? (1u << tile.maskt) : ((tile.lrt - tile.ult) >> 2) + 1;
        w = std::clamp<uint32_t>(w, 1, 1024);
        h = std::clamp<uint32_t>(h, 1, 1024);
        // A clamped tile larger than its mask: the RDP wraps (or mirrors) within the mask and clamps at
        // the tile's edge, which the GPU can't do in one sampler. Up to 256 texels it's decoded at the
        // tile's size with the wrapping done here, and clamped (bind_tile). Pikachu's back stripes
        // (mirrored in 32, clamped at 64) otherwise repeated past the edge: a stripe across his chest.
        uint32_t ext_s = tile.lrs >= tile.uls ? ((tile.lrs - tile.uls) >> 2) + 1 : 1;
        uint32_t ext_t = tile.lrt >= tile.ult ? ((tile.lrt - tile.ult) >> 2) + 1 : 1;
        bool unroll_s = tile_unrolled(tile.cms, tile.masks, ext_s), unroll_t = tile_unrolled(tile.cmt, tile.maskt, ext_t);
        uint32_t ew = unroll_s ? ext_s : w, eh = unroll_t ? ext_t : h; // decoded extent
        bool mir_s = unroll_s && (tile.cms & G_TX_MIRROR), mir_t = unroll_t && (tile.cmt & G_TX_MIRROR);
        auto src_col = [&](uint32_t x) {
            x = std::min(x, ew - 1);
            if (!unroll_s) return x;
            uint32_t m = x & (w - 1);
            return mir_s && (x & w) ? w - 1 - m : m;
        };
        auto src_row = [&](uint32_t y) {
            y = std::min(y, eh - 1);
            if (!unroll_t) return y;
            uint32_t m = y & (h - 1);
            return mir_t && (y & h) ? h - 1 - m : m;
        };
        uint32_t unroll_bits = (unroll_s ? 1u << 24 : 0) | (unroll_t ? 1u << 25 : 0) | (mir_s ? 1u << 26 : 0) | (mir_t ? 1u << 27 : 0);

        uint32_t row_bytes = (w * bytes_per_texel_x2(tile.siz) + 1) / 2;
        uint32_t stride, start;
        uint32_t offset = (uint32_t)(tile.tmem - load_tmem) * 8;
        if (load->src_stride) {
            stride = load->src_stride;
            uint32_t tline = load->tmem_line ? load->tmem_line : stride;
            start = load->src + (offset / tline) * stride + (offset % tline);
        }
        else {
            // RGBA32 sits in TMEM as two 16-bit halves (red/green low, blue/alpha high), so its
            // line and TMEM offset count half the bytes each row takes in RDRAM.
            uint32_t scale = tile.siz == G_IM_SIZ_32b ? 2 : 1;
            stride = tile.line ? tile.line * 8u * scale : row_bytes;
            start = load->src + offset * scale;
        }
        uint8_t tlut_type = (other_mode_h >> G_TT_SHIFT) & 3;
        if (!offscreen_inflight.empty() || frames.size() > 1) sync_offscreen_reads(start, stride * h);
        if (false && !offscreen_images.empty()) { // render-to-texture: off (the backend writes images back to RDRAM)
            if (auto rt = offscreen_texture(start)) {
                out_w = rt->width;
                out_h = rt->height;
                return rt;
            }
        }
        // (32-bit texels: TMEM keeps them as two 16-bit halves, so the swap covers 8 bytes of RDRAM.)
        uint32_t odd_xor = (!load->src_stride && load->odd_rows_swapped) ? (tile.siz == G_IM_SIZ_32b ? 8 : 4) : 0;

        uint32_t pw = next_pow2(ew), ph = next_pow2(eh);
        out_w = pw;
        out_h = ph;

        // The same texture is bound many times per frame, and hashing its source bytes for every bind
        // took over half of the interpretation time. The game doesn't rewrite texture memory while its
        // display list is processed, so within one display list the result for the same source,
        // layout and palette is reused.
        // Bind cache: the same source, layout and palette usually gives the same texture from frame to
        // frame. A repeat bind costs one lookup and, once per display list, the 16-word fingerprint
        // check (see source_content_hash); the full key is only rebuilt when that fails.
        Phase phase_hash(PHASE_TEX_HASH);
        uint64_t pal = tile.fmt == G_IM_FMT_CI ? palette_hash : 0;
        uint32_t params[8] = { start, stride, unroll_bits | odd_xor << 16 | (uint32_t)tile.fmt << 8 | tile.siz, ew, eh, (uint32_t)tile.palette << 2 | tlut_type,
                               (uint32_t)pal, (uint32_t)(pal >> 32) };
        uint64_t bind_key = mix_words(params, 8);
        tex_binds++;
        auto cached = bind_cache.find(bind_key);
        if (cached != bind_cache.end()) {
            BindEntry& e = cached->second;
            if (e.seen_dl == dl_count) {
                tex_memo_hits++;
                return e.image;
            }
            if (!n64_tex_nocache && !e.volatile_src && dl_count - e.verified_dl < 8 && e.fingerprint == source_fingerprint(start, stride, row_bytes, h)) {
                tex_memo_hits++;
                e.seen_dl = dl_count;
                e.image->last_used = (uint32_t)dl_count;
                return e.image;
            }
        }

        // Key on everything that affects the decoded result, including the source bytes.
        uint64_t content = source_content_hash(start, stride, row_bytes, w, h, params[2]);
        uint64_t key = mix_words(params, 8) ^ (content * 0x9E3779B97F4A7C15ull);

        auto it = texture_cache.find(key);
        uint32_t fingerprint = source_fingerprint(start, stride, row_bytes, h);
        if (bind_cache.size() > 8192) bind_cache.clear();
        if (it != texture_cache.end()) {
            tex_cache_hits++;
            it->second->last_used = (uint32_t)dl_count;
            bind_cache[bind_key] = { it->second, fingerprint, dl_count, dl_count, last_source_volatile };
            return it->second;
        }

        // (Clearing everything as soon as the limit was reached, as before, re-decoded every texture
        // of a busy area each frame.)
        trim_texture_cache();
        Phase phase_decode(PHASE_TEX_DECODE);
        tex_decodes++;
        bool ci = tile.fmt == G_IM_FMT_CI && (tile.siz == G_IM_SIZ_4b || tile.siz == G_IM_SIZ_8b);
        uint64_t index_key = 0;
        if (ci) {
            uint32_t iparams[8] = { start, stride, unroll_bits | odd_xor << 16 | (uint32_t)tile.siz, ew, eh, (uint32_t)tile.palette << 2 | tlut_type,
                                    (uint32_t)content, (uint32_t)(content >> 32) };
            index_key = mix_words(iparams, 8);
        }
        if (n64_texlog_remaining > 0) {
            n64_texlog_remaining = n64_texlog_remaining - 1;
            fprintf(stderr, "[texlog] dl %d src %06X stride %u %ux%u fmt %u siz %u pal %u tt %u palhash %08X content %08X\n", dl_count,
                (unsigned)start, (unsigned)stride, (unsigned)w, (unsigned)h, (unsigned)tile.fmt, (unsigned)tile.siz,
                (unsigned)tile.palette, (unsigned)tlut_type, (unsigned)pal, (unsigned)content);
        }

        auto img = std::make_shared<TexImage>();
        img->key = key;
        img->width = (uint16_t)pw;
        img->height = (uint16_t)ph;
        img->texels.resize(pw * ph);
        if (ci) {
            trim_index_cache();
            IndexImage& ii = index_cache[index_key];
            if (ii.indices.size() != (size_t)pw * ph) {
                index_bytes += (size_t)pw * ph - ii.indices.size();
                ii.indices.resize(pw * ph);
                for (uint32_t y = 0; y < ph; y++) {
                    uint32_t sy = src_row(y); // pad by repeating the edge
                    uint32_t row = start + sy * stride;
                    uint32_t xr = (sy & 1) ? odd_xor : 0;
                    for (uint32_t x = 0; x < pw; x++) {
                        uint32_t sx = src_col(x);
                        uint8_t n;
                        if (tile.siz == G_IM_SIZ_4b) {
                            uint8_t b = read_u8(row + ((sx / 2) ^ xr));
                            n = (sx & 1) ? (b & 0xF) : (b >> 4);
                        } else {
                            n = read_u8(row + (sx ^ xr));
                        }
                        ii.indices[tiled_offset(x, y, pw, ph)] = n;
                    }
                }
            }
            ii.used_dl = dl_count;
            // The palette as texels, then one lookup per texel.
            uint32_t table[256];
            uint32_t base = tile.siz == G_IM_SIZ_4b ? tile.palette * 16u : 0u, count = tile.siz == G_IM_SIZ_4b ? 16u : 256u;
            for (uint32_t i = 0; i < count; i++) {
                uint16_t c = palette[(base + i) & 0xFF];
                table[i] = tlut_type == G_TT_IA16 ? ia16(c) : rgba5551(c);
            }
            const uint8_t* idx = ii.indices.data();
            uint32_t* out = img->texels.data();
            for (size_t i = 0, n = img->texels.size(); i < n; i++) out[i] = table[idx[i] & (count - 1)];
            img->gpu_slot = index_key | 1; // never 0
        } else {
            for (uint32_t y = 0; y < ph; y++) {
                uint32_t sy = src_row(y); // pad by repeating the edge
                uint32_t row = start + sy * stride;
                uint32_t xr = (sy & 1) ? odd_xor : 0;
                for (uint32_t x = 0; x < pw; x++) {
                    uint32_t sx = src_col(x);
                    img->texels[tiled_offset(x, y, pw, ph)] = decode_texel(tile, row, sx, tlut_type, xr);
                }
            }
        }
        img->fmt = tile.fmt;
        img->siz = tile.siz;
        img->src = start;
        img->stride = stride;
        img->src_w = (uint16_t)w;
        img->src_h = (uint16_t)h;
        // Diagnostics for draw dumps; skipped for palette textures, which can be rebuilt every frame.
        size_t opaque = 0, luma = 0;
        if (!ci) {
            for (uint32_t t : img->texels) {
                opaque += (t & 0xFF) >= 128;
                luma += ((t >> 24) + ((t >> 16) & 0xFF) + ((t >> 8) & 0xFF)) / 3;
            }
        }
        img->opaque_pct = ci ? 255 : (uint8_t)(opaque * 100 / img->texels.size());
        img->mean_luma = ci ? 255 : (uint8_t)(luma / img->texels.size());
        if (tile.fmt == G_IM_FMT_CI && img->mean_luma < 8 && logged_dark_ci < 4) {
            logged_dark_ci++;
            fprintf(stderr, "[gfx] dark CI texture %ux%u siz=%u pal=%u tlut=%u start=0x%06X; palette[0..7]:", (unsigned)w, (unsigned)h,
                (unsigned)tile.siz, (unsigned)tile.palette, (unsigned)tlut_type, (unsigned)start);
            for (int i = 0; i < 8; i++) fprintf(stderr, " %04X", palette[(tile.palette * 16 + i) & 0xFF]);
            fprintf(stderr, " (last LOADTLUT base %u count %u from 0x%06X)\n", (unsigned)last_tlut_base, (unsigned)last_tlut_count, (unsigned)last_tlut_src);
        }
        img->last_used = (uint32_t)dl_count;
        auto& slot = texture_cache[key];
        if (slot) texture_bytes -= image_bytes(*slot);
        slot = img;
        texture_bytes += image_bytes(*img);
        bind_cache[bind_key] = { img, fingerprint, dl_count, dl_count, last_source_volatile };
        return img;
    }

    TexBinding bind_tile(int tile_index, float& norm_w, float& norm_h, float& off_s, float& off_t,
                         float& shift_s, float& shift_t) {
        const Tile& tile = tiles[tile_index & 7];
        TexBinding b;
        uint32_t pw = 8, ph = 8;
        b.image = get_texture(tile, pw, ph);
        // The RDP clamps at the tile size and masks (wraps) within it: a clamped tile larger than its
        // mask repeats inside the tile (Pokemon Snap's terrain: 64-texel masks on tiles up to 1024
        // wide). Clamp only when the tile fits in one mask period.
        auto wrap = [](uint32_t cm, uint32_t mask, uint32_t lo, uint32_t hi) {
            uint32_t extent = hi >= lo ? ((hi - lo) >> 2) + 1 : 1;
            bool clamp = !mask || ((cm & G_TX_CLAMP) && extent <= (1u << mask) + 1) || tile_unrolled(cm, mask, extent);
            return clamp ? TexWrap::Clamp : (cm & G_TX_MIRROR) ? TexWrap::Mirror : TexWrap::Repeat;
        };
        b.wrap_s = wrap(tile.cms, tile.masks, tile.uls, tile.lrs);
        b.wrap_t = wrap(tile.cmt, tile.maskt, tile.ult, tile.lrt);
        b.linear = ((other_mode_h >> G_TF_SHIFT) & 3) != G_TF_POINT;
        norm_w = (float)pw;
        norm_h = (float)ph;
        off_s = tile.uls / 4.0f;
        off_t = tile.ult / 4.0f;
        auto shift = [](uint8_t sh) { return sh == 0 ? 1.0f : sh <= 10 ? 1.0f / (1 << sh) : (float)(1 << (16 - sh)); };
        shift_s = shift(tile.shifts);
        shift_t = shift(tile.shiftt);
        return b;
    }

    // --- Draw state ---

    struct UvMap {
        float w = 1, h = 1, off_s = 0, off_t = 0, shift_s = 1, shift_t = 1;
        const RtInfo* rt = nullptr; // render-to-texture: map into the off-screen target (to_gpu)
    } uv_map[2];
    float uv_inv[2][2] = { { 1, 1 }, { 1, 1 } }; // 1 / uv_map[i].w, .h, refreshed before emitting
    struct LastRect { // see rect_state_changed
        const FrameData* frame = nullptr;
        size_t draw_count = 0;
        int tile = -1;
        uint32_t cycle = 0, fill_color = 0;
        bool textured = false;
        UvMap uv[2];
    } last_rect;
    // The last untextured rectangle (N64 pixels): a photo's mat when a photo is drawn on it.
    struct PlainRect {
        const FrameData* frame = nullptr;
        size_t draw = 0;
        float ulx = 0, uly = 0, lrx = 0, lry = 0;
    } plain_rect;

    void refresh_uv_inv() {
        for (int i = 0; i < 2; i++) {
            uv_inv[i][0] = 1.0f / uv_map[i].w;
            uv_inv[i][1] = 1.0f / uv_map[i].h;
        }
    }

    void decode_combiner(CombinerCycle out[2]) const {
        uint32_t w0 = combine_w0, w1 = combine_w1;
        uint8_t a[2] = { (uint8_t)((w0 >> 20) & 0xF), (uint8_t)((w0 >> 5) & 0xF) };
        uint8_t c[2] = { (uint8_t)((w0 >> 15) & 0x1F), (uint8_t)(w0 & 0x1F) };
        uint8_t aa[2] = { (uint8_t)((w0 >> 12) & 7), (uint8_t)((w1 >> 21) & 7) };
        uint8_t ac[2] = { (uint8_t)((w0 >> 9) & 7), (uint8_t)((w1 >> 18) & 7) };
        uint8_t b[2] = { (uint8_t)((w1 >> 28) & 0xF), (uint8_t)((w1 >> 24) & 0xF) };
        uint8_t d[2] = { (uint8_t)((w1 >> 15) & 7), (uint8_t)((w1 >> 6) & 7) };
        uint8_t ab[2] = { (uint8_t)((w1 >> 12) & 7), (uint8_t)((w1 >> 3) & 7) };
        uint8_t ad[2] = { (uint8_t)((w1 >> 9) & 7), (uint8_t)(w1 & 7) };
        for (int i = 0; i < 2; i++) {
            out[i].rgb[0] = cc_rgb_a[a[i]];
            out[i].rgb[1] = cc_rgb_b[b[i]];
            out[i].rgb[2] = cc_rgb_c[c[i]];
            out[i].rgb[3] = cc_rgb_d[d[i]];
            out[i].alpha[0] = cc_alpha_abd[aa[i]];
            out[i].alpha[1] = cc_alpha_abd[ab[i]];
            out[i].alpha[2] = cc_alpha_c[ac[i]];
            out[i].alpha[3] = cc_alpha_abd[ad[i]];
        }
        for (int k = 0; k < 4; k++) {
            out[1].rgb[k] = swap_tex(out[1].rgb[k]);
            out[1].alpha[k] = swap_tex(out[1].alpha[k]);
        }
    }

    // RDP blender: per cycle, color = (P*A + M*B) / (A + B), with
    //   P, M: 0 = combiner color (IN), 1 = framebuffer (MEM), 2 = blend color, 3 = fog color
    //   A:    0 = combiner alpha, 1 = fog alpha, 2 = shade alpha, 3 = 0
    //   B:    0 = 1 - A, 1 = framebuffer coverage, 2 = 1, 3 = 0
    // The cycle that writes to memory decides how the pixel lands; in 2-cycle mode cycle 1 is
    // usually fog (applied in the combiner's place, not emulated yet), so the second cycle is used.
    BlendMode decode_blender(bool two_cycle, uint8_t& const_alpha) const {
        uint32_t m = other_mode_l >> 16;
        uint32_t p, a, mm, b;
        if (two_cycle) {
            p = (m >> 12) & 3; a = (m >> 8) & 3; mm = (m >> 4) & 3; b = m & 3;
        }
        else {
            p = (m >> 14) & 3; a = (m >> 10) & 3; mm = (m >> 6) & 3; b = (m >> 2) & 3;
        }
        const_alpha = 255;
        bool p_in = p == 0, m_mem = mm == 1, m_in = mm == 0;
        if (p_in && m_in) return BlendMode::Opaque;
        if (p_in && m_mem) {
            if (a == 3 && b == 2) return BlendMode::KeepMemory;         // IN*0 + MEM*1
            if (a == 3 && b == 3) return BlendMode::Opaque;             // degenerate
            if (b == 1) return BlendMode::Opaque;                       // coverage (antialiasing) modes
            if (b == 2) return BlendMode::Opaque;                       // IN*a + MEM*1: additive, approximated
            if (a == 0 || a == 2) return BlendMode::Alpha;              // combiner/shade alpha
            if (a == 1) {                                               // fog alpha (constant)
                const_alpha = fog_color[3];
                return BlendMode::ConstantAlpha;
            }
        }
        if (p == 1 && m_in) {
            if (a == 3 && b == 2) return BlendMode::Opaque;             // MEM*0 + IN*1
        }
        if (p == 1 && m_mem) return BlendMode::KeepMemory;
        // Blend/fog color sources and other rare forms: treat as alpha blending.
        return BlendMode::Alpha;
    }

    void log_combiner() {
        uint64_t k = ((uint64_t)combine_w0 << 32) | combine_w1;
        if (logged_combiners.size() < 64 && logged_combiners.insert(k).second) {
            fprintf(stderr, "[gfx] combiner %06X:%08X cycle=%u othermode_l=%08X\n",
                (unsigned)(combine_w0 & 0xFFFFFF), (unsigned)combine_w1,
                (unsigned)((other_mode_h >> G_CYC_SHIFT) & 3), (unsigned)other_mode_l);
        }
    }

    // Starts a new draw call with the current state if anything changed since the last one.
    DrawCmd* begin_draw(int rect_tile, bool is_rect) {
        if (!frame) select_frame(cimg_addr);
        if (!state_dirty && !frame->draws.empty() && !is_rect) {
            return &frame->draws.back();
        }
        Phase phase(PHASE_STATE);
        state_dirty = is_rect; // a rect always forces a fresh draw for whatever follows
        log_combiner();

        DrawCmd d{};
        d.first_vertex = (uint32_t)frame->vertices.size();
        uint32_t cycle = (other_mode_h >> G_CYC_SHIFT) & 3;
        d.two_cycle = cycle == G_CYC_2CYCLE;
        d.copy_mode = cycle == G_CYC_COPY;
        d.fill_mode = cycle == G_CYC_FILL;
        decode_combiner(d.cycles);
        memcpy(d.prim, prim, 4);
        memcpy(d.env, env, 4);
        d.prim_lod_frac = prim_lod_frac;

        bool textured = is_rect || texture_on;
        if (textured && !d.fill_mode) {
            int tile0 = is_rect ? rect_tile : texture_tile;
            bool need0 = d.copy_mode || uses(d.cycles[0], CC_TEX0, CC_TEX0_A) || (d.two_cycle && uses(d.cycles[1], CC_TEX0, CC_TEX0_A));
            bool need1 = !d.copy_mode && (uses(d.cycles[0], CC_TEX1, CC_TEX1_A) || (d.two_cycle && uses(d.cycles[1], CC_TEX1, CC_TEX1_A)));
            if (need0) {
                UvMap& m = uv_map[0];
                d.tex[0] = bind_tile(tile0, m.w, m.h, m.off_s, m.off_t, m.shift_s, m.shift_t);
                m.rt = rt_of(d.tex[0]);
            }
            if (need1) {
                UvMap& m = uv_map[1];
                d.tex[1] = bind_tile(tile0 + 1, m.w, m.h, m.off_s, m.off_t, m.shift_s, m.shift_t);
                m.rt = rt_of(d.tex[1]);
            }
        }

        // Blending / alpha test / depth from the RDP other modes.
        d.other_mode_l = other_mode_l;
        if (d.copy_mode || d.fill_mode) {
            d.blend = BlendMode::Opaque;
        }
        else {
            d.blend = decode_blender(d.two_cycle, d.blend_alpha);
        }
        d.alpha_test = d.copy_mode || (other_mode_l & G_AC_MASK) == G_AC_THRESHOLD || (other_mode_l & CVG_X_ALPHA);
        d.alpha_ref = (other_mode_l & G_AC_MASK) == G_AC_THRESHOLD ? blend_color[3] : 0x4D; // ~0.3 for coverage cutouts
        // Coverage x alpha still cuts out low-alpha texels when a threshold of 0 is also set (Pokemon
        // Snap's tree leaves: TEX_EDGE with G_AC_THRESHOLD and blend alpha 0 drew as solid squares).
        if ((other_mode_l & CVG_X_ALPHA) && d.alpha_ref < 0x4D) d.alpha_ref = 0x4D;
        if (d.copy_mode && (other_mode_l & G_AC_MASK) != G_AC_THRESHOLD) d.alpha_ref = 1;
        bool zbuf = (geometry_mode & G_ZBUFFER) && !is_rect;
        bool prim_z = is_rect && rect_prim_w > 0.0f;
        d.depth_test = (zbuf || prim_z) && (other_mode_l & Z_CMP);
        d.depth_write = (zbuf || prim_z) && (other_mode_l & Z_UPD);
        d.decal = (other_mode_l & ZMODE_MASK) == ZMODE_DEC;
        d.geometry_mode = geometry_mode;

        if (cimg_offscreen) { // N64 pixels 1:1 (see DrawCmd::offscreen_map)
            d.offscreen_map = true;
            d.scissor[0] = (uint16_t)std::clamp<int>(scissor[0], 0, 512);
            d.scissor[1] = (uint16_t)std::clamp<int>(scissor[1], 0, 256);
            d.scissor[2] = (uint16_t)std::clamp<int>(scissor[2], 0, 512);
            d.scissor[3] = (uint16_t)std::clamp<int>(scissor[3], 0, 256);
            frame->draws.push_back(std::move(d));
            return &frame->draws.back();
        }
        // Scissor: N64 pixels -> 3DS top-screen pixels.
        float fb_wf = (float)cimg_width;
        float scale = top_h / (fb_wf * 0.75f);
        float x0 = (top_w - fb_wf * scale) * 0.5f;
        d.scissor[0] = (uint16_t)std::clamp(x0 + scissor[0] * scale, 0.0f, top_w);
        d.scissor[1] = (uint16_t)std::clamp(scissor[1] * scale, 0.0f, top_h);
        d.scissor[2] = (uint16_t)std::clamp(x0 + scissor[2] * scale, 0.0f, top_w);
        d.scissor[3] = (uint16_t)std::clamp(scissor[3] * scale, 0.0f, top_h);
        if (n64_widescreen && scissor[0] == 0 && scissor[2] >= cimg_width) {
            d.scissor[0] = 0;
            d.scissor[2] = (uint16_t)top_w;
        }

        frame->draws.push_back(std::move(d)); // move: copying the texture pointers costs atomic ops
        return &frame->draws.back();
    }

    void emit(const Vertex& v, DrawCmd* d) {
        frame->vertices.push_back(to_gpu(v));
        d->vertex_count++;
    }

    // HUD gauges and the minimap go to the bottom screen. The game draws them in its 2D UI pass
    // (no depth test, orthographic): life in the lower left corner, weapon energy in the lower
    // right, the minimap in the upper left. Returns 1, 2 or 3 when every vertex lies in one of those
    // areas, else 0.
    bool hud_life_shown = false, hud_energy_shown = false, hud_map_shown = false; // from game RAM per display list

    int hud_region(const DrawCmd* d, const Vertex* const* vs, int n) const {
        if (!hud_life_shown && !hud_energy_shown && !hud_map_shown) return 0;
        if (d->depth_test || d->depth_write || d->depth_clear || d->fill_mode) return 0;
        // Textured pieces must come from the HUD's texture sheet (RDRAM 0x162000-0x166C00: gauges,
        // minimap tiles); otherwise e.g. the opening narration's ocean, drawn the same 2D way, went too.
        if (const TexImage* img = d->tex[0].image.get()) {
            if (img->src < 0x162000 || img->src >= 0x166C00) return 0;
        }
        int region = 0;
        for (int i = 0; i < n; i++) {
            const Vertex& v = *vs[i];
            if (std::fabs(v.w - 1.0f) > 0.001f) return 0;
            float x = (v.x + 1.0f) * 200.0f, y = (1.0f - v.y) * 120.0f; // top-screen pixels
            // Measured extents: life x 47-90 y 136-234, energy x 311-338 y 165-232, minimap with
            // its frame x 53-110 y 35-92 (the dialogue box's left edge, at x 106-110, starts higher).
            int r = (hud_map_shown && x >= 52.5f && x <= 110.5f && y >= 34.5f && y <= 92.5f) ? 3
                  : y < 125.0f || y > 244.0f ? 0
                  : (hud_life_shown && x >= 38.0f && x < 100.0f) ? 1
                  : (hud_energy_shown && x >= 300.0f && x < 350.0f) ? 2 : 0;
            if (r == 0 || (region && r != region)) return 0;
            region = r;
        }
        return region;
    }

    int overlay_source = -1; // index of the top-screen draw the last overlay draw copied

    // Adds vertices to the bottom-screen overlay instead of the top screen.
    void emit_overlay(const Vertex* const* vs, int n, DrawCmd* d, int region) {
        int source = (int)(d - frame->draws.data());
        if (frame->overlay_draws.empty() || overlay_source != source || frame->overlay_draws.back().overlay_region != region) {
            DrawCmd copy = *d;
            copy.first_vertex = (uint32_t)frame->overlay_vertices.size();
            copy.vertex_count = 0;
            copy.overlay_region = (uint8_t)region;
            frame->overlay_draws.push_back(std::move(copy));
            overlay_source = source;
        }
        DrawCmd& o = frame->overlay_draws.back();
        for (int i = 0; i < n; i++) frame->overlay_vertices.push_back(to_gpu(*vs[i]));
        o.vertex_count += n;
    }

    GpuVertex to_gpu(const Vertex& v) {
        GpuVertex g;
        // Rotate into the 240x400 portrait render target used for the top screen.
        g.pos[0] = v.y;
        g.pos[1] = -v.x;
        g.pos[2] = v.z;
        g.pos[3] = v.w;
        for (int i = 0; i < 2; i++) {
            const UvMap& m = uv_map[i];
            float* uv = i == 0 ? g.uv0 : g.uv1;
            if (m.rt) {
                // Render-to-texture: image pixel (px, py) is at (px, py) of the target's 512x256 area,
                // which in the 256x512 portrait texture is x = 256 - py, rows from the bottom 512 - px.
                float px = m.rt->x0 + (v.u * m.shift_s - m.off_s), py = m.rt->y0 + (v.v * m.shift_t - m.off_t);
                uv[0] = (256.0f - py) / 256.0f;
                uv[1] = px / 512.0f;
                continue;
            }
            float s = (v.u * m.shift_s - m.off_s) * uv_inv[i][0];
            float t = (v.v * m.shift_t - m.off_t) * uv_inv[i][1];
            uv[0] = s;
            uv[1] = t; // rows are stored flipped (tiled_offset), so t maps directly
        }
        memcpy(g.color, v.color, 4);
        return g;
    }

    void triangle(int i0, int i1, int i2) {
        Phase phase(PHASE_TRI);
        bool tracing = n64_dltrace_dl == dl_count;
        if (i0 >= vertex_buffer_size || i1 >= vertex_buffer_size || i2 >= vertex_buffer_size) {
            if (tracing) fprintf(stderr, "[tri] dropped: index %d %d %d\n", i0, i1, i2);
            return;
        }
        const Vertex& a = vertices[i0];
        const Vertex& b = vertices[i1];
        const Vertex& c = vertices[i2];
        if (tracing) {
            fprintf(stderr, "[tri] %d %d %d: (%.0f %.0f %.0f %.0f) (%.0f %.0f %.0f %.0f) (%.0f %.0f %.0f %.0f) gm %06lX\n", i0, i1, i2,
                a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w, c.x, c.y, c.z, c.w, (unsigned long)geometry_mode);
        }

        // Back/front face culling in screen space when all vertices are in front of the camera.
        if ((geometry_mode & G_CULL_BOTH) && a.w > 0 && b.w > 0 && c.w > 0 && !n64_debug_nocull) {
            float ax = a.x / a.w, ay = a.y / a.w;
            float cross = (b.x / b.w - ax) * (c.y / c.w - ay) - (b.y / b.w - ay) * (c.x / c.w - ax);
            bool culled = (geometry_mode & G_CULL_BOTH) == G_CULL_BOTH || ((geometry_mode & G_CULL_FRONT) && cross >= 0) ||
                          ((geometry_mode & G_CULL_BACK) && cross <= 0);
            if (culled) {
                if (tracing) fprintf(stderr, "[tri] culled (cross %.4f)\n", cross);
                return;
            }
        }

        DrawCmd* d = begin_draw(0, false);
        if (frame->vertices.size() > max_frame_vertices) return;
        refresh_uv_inv();

        // Flat shading uses the first vertex's color.
        Vertex fa = a, fb = b, fc = c;
        if (!(geometry_mode & G_SHADING_SMOOTH)) {
            memcpy(fb.color, a.color, 3);
            memcpy(fc.color, a.color, 3);
        }
        if (cimg_real_width >= 64 && !cimg_offscreen) center_raster(fa, fb, fc, d->depth_test, d->depth_write);
        if (cimg_offscreen && cimg_real_width <= 64) depth_raster(fa, fb, fc, d->depth_test, d->depth_write);
        const Vertex* tri[3] = { &fa, &fb, &fc };
        if (int region = hud_region(d, tri, 3)) {
            emit_overlay(tri, 3, d, region);
            return;
        }
        if (inside_guard_band(fa) && inside_guard_band(fb) && inside_guard_band(fc)) {
            emit(fa, d); emit(fb, d); emit(fc, d);
            return;
        }
        clip_and_emit(fa, fb, fc, d);
    }

    // Guard-band clipping, as the RSP does it. The PICA200 clips only depth, and its rasterizer
    // can't take vertices far off screen or just in front of the camera (huge x/w, y/w): such
    // triangles were dropped, leaving straight-edged holes in Pokemon Snap's ground near the camera.
    // Triangles reaching past guard_band screens (or to w <= clip_w) are clipped in clip space.
    static constexpr float guard_band = 2.0f;
    static constexpr float clip_w = 0.01f;

    static bool inside_guard_band(const Vertex& v) {
        float k = guard_band * v.w;
        return v.w >= clip_w && v.x <= k && v.x >= -k && v.y <= k && v.y >= -k;
    }

    static float plane_distance(const Vertex& v, int plane) {
        switch (plane) {
            case 0: return v.w - clip_w;
            case 1: return guard_band * v.w - v.x;
            case 2: return guard_band * v.w + v.x;
            case 3: return guard_band * v.w - v.y;
            default: return guard_band * v.w + v.y;
        }
    }

    static Vertex lerp_vertex(const Vertex& p, const Vertex& q, float t) {
        Vertex r;
        r.x = p.x + (q.x - p.x) * t;
        r.y = p.y + (q.y - p.y) * t;
        r.z = p.z + (q.z - p.z) * t;
        r.w = p.w + (q.w - p.w) * t;
        r.u = p.u + (q.u - p.u) * t;
        r.v = p.v + (q.v - p.v) * t;
        r.screen_z = p.screen_z + (q.screen_z - p.screen_z) * t;
        for (int i = 0; i < 4; i++) r.color[i] = (uint8_t)std::lround(p.color[i] + (q.color[i] - p.color[i]) * t);
        return r;
    }

    void clip_and_emit(const Vertex& a, const Vertex& b, const Vertex& c, DrawCmd* d) {
        Vertex poly[2][9];
        int n = 3;
        poly[0][0] = a; poly[0][1] = b; poly[0][2] = c;
        int cur = 0;
        for (int plane = 0; plane < 5 && n >= 3; plane++) {
            const Vertex* in = poly[cur];
            Vertex* out = poly[cur ^ 1];
            int m = 0;
            for (int i = 0; i < n; i++) {
                const Vertex& p = in[i];
                const Vertex& q = in[(i + 1) % n];
                float dp = plane_distance(p, plane), dq = plane_distance(q, plane);
                if (dp >= 0) out[m++] = p;
                if ((dp >= 0) != (dq >= 0) && m < 9) out[m++] = lerp_vertex(p, q, dp / (dp - dq));
            }
            n = m;
            cur ^= 1;
        }
        for (int i = 1; i + 1 < n; i++) { // fan
            emit(poly[cur][0], d); emit(poly[cur][i], d); emit(poly[cur][i + 1], d);
        }
    }

    bool depth_clear_pending = false; // a z-buffer fill waiting for its color buffer (see rect)

    // Pokemon Snap's focus detection (app_level/pokemon_detect.c): after drawing each Pokemon, the
    // game copies the framebuffer's center 8x8 tile (x 156-163, y 116-123) into an 8-pixel-wide
    // buffer with a COPY-mode texture rectangle, then counts how many of the 64 pixels changed from
    // the copy before: 32 or more and that Pokemon is in focus. The framebuffer isn't in RDRAM here,
    // so the tile is followed in software: each triangle drawn is rasterized over those 64 pixels
    // with a depth test (on 1/w), and the copy writes a value per pixel that identifies what's on top.
    static constexpr int center_x = 156, center_y = 116;
    uint32_t center_id[64] = {};
    float center_depth[64] = {};
    uint32_t center_serial = 0;

    void center_reset() {
        for (int i = 0; i < 64; i++) {
            center_id[i] = 0;
            center_depth[i] = 0.0f; // 1/w: 0 = infinitely far
        }
    }

    void center_raster(const Vertex& a, const Vertex& b, const Vertex& c, bool depth_test, bool depth_write) {
        if (a.w <= 0.0f || b.w <= 0.0f || c.w <= 0.0f) return;
        float fb_wf = (float)cimg_width, fb_hf = fb_wf * 0.75f;
        float kx = (top_h * 4.0f / 3.0f / top_w) / (fb_wf * 0.5f);
        const Vertex* v[3] = { &a, &b, &c };
        float sx[3], sy[3], iw[3];
        for (int i = 0; i < 3; i++) {
            iw[i] = 1.0f / v[i]->w;
            sx[i] = v[i]->x * iw[i] / kx + fb_wf * 0.5f;
            sy[i] = fb_hf * 0.5f * (1.0f - v[i]->y * iw[i]);
        }
        float minx = std::min({ sx[0], sx[1], sx[2] }), maxx = std::max({ sx[0], sx[1], sx[2] });
        float miny = std::min({ sy[0], sy[1], sy[2] }), maxy = std::max({ sy[0], sy[1], sy[2] });
        if (maxx < center_x || minx > center_x + 8 || maxy < center_y || miny > center_y + 8) return;
        float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
        if (std::fabs(area) < 1e-6f) return;
        uint32_t id = ++center_serial;
        for (int j = 0; j < 8; j++) {
            float py = center_y + j + 0.5f;
            for (int i = 0; i < 8; i++) {
                float px = center_x + i + 0.5f;
                float w0 = ((sx[1] - px) * (sy[2] - py) - (sy[1] - py) * (sx[2] - px)) / area;
                float w1 = ((sx[2] - px) * (sy[0] - py) - (sy[2] - py) * (sx[0] - px)) / area;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float depth = w0 * iw[0] + w1 * iw[1] + w2 * iw[2];
                int k = j * 8 + i;
                if (depth_test && depth < center_depth[k]) continue;
                center_id[k] = id;
                if (depth_write) center_depth[k] = depth;
            }
        }
    }

    // Pokemon Snap scores photos at Camera Check from depth: it re-renders each photo into 64x48
    // buffers with one z-buffer per Pokemon and counts, on the CPU, the pixels whose depth isn't the
    // cleared 0xFFFC (score.c, func_800A007C). Depth for small off-screen targets (64 wide or less)
    // is therefore also rasterized here into the z image in RDRAM. Values only have to be consistent
    // and ordered: screen z (0..1023) is interpolated across the screen and stored as z * 63, below
    // 0xFFFC, with the N64's less-than test.
    uint32_t depth_raster_pixels = 0, depth_raster_tris = 0, depth_raster_logs = 0; // development
    void depth_raster(const Vertex& a, const Vertex& b, const Vertex& c, bool depth_test, bool depth_write) {
        depth_raster_tris++;
        if (depth_raster_logs < 6 && depth_raster_tris % 200 == 1) {
            depth_raster_logs++;
            fprintf(stderr, "[zr] dl %d cimg %06X w %u/%u zimg %06X test %d write %d v (%.2f %.2f %.2f %.1f sz %.1f) sc %u,%u,%u,%u\n", dl_count,
                (unsigned)cimg_addr, (unsigned)cimg_width, (unsigned)cimg_real_width, (unsigned)zimg_addr, (int)depth_test, (int)depth_write,
                a.x, a.y, a.z, a.w, a.screen_z, scissor[0], scissor[1], scissor[2], scissor[3]);
        }
        if (!depth_write || !zimg_addr || a.w <= 0.0f || b.w <= 0.0f || c.w <= 0.0f) return;
        int fb_w = (int)cimg_real_width, fb_h = fb_w * 3 / 4;
        const Vertex* v[3] = { &a, &b, &c };
        float sx[3], sy[3], sz[3];
        for (int i = 0; i < 3; i++) { // off-screen vertices map N64 pixels 1:1 (see load_vertices)
            float iw = 1.0f / v[i]->w;
            sx[i] = (v[i]->x * iw + 1.0f) * 256.0f;
            sy[i] = (1.0f - v[i]->y * iw) * 128.0f;
            sz[i] = std::clamp(v[i]->screen_z, 0.0f, 1023.0f);
        }
        float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
        if (std::fabs(area) < 1e-6f) return;
        int x0 = std::max<int>({ 0, (int)scissor[0], (int)std::floor(std::min({ sx[0], sx[1], sx[2] })) });
        int x1 = std::min<int>({ fb_w - 1, (int)scissor[2] - 1, (int)std::ceil(std::max({ sx[0], sx[1], sx[2] })) });
        int y0 = std::max<int>({ 0, (int)scissor[1], (int)std::floor(std::min({ sy[0], sy[1], sy[2] })) });
        int y1 = std::min<int>({ fb_h - 1, (int)scissor[3] - 1, (int)std::ceil(std::max({ sy[0], sy[1], sy[2] })) });
        if (x0 > x1 || y0 > y1 || !in_range(zimg_addr, (uint32_t)(fb_w * fb_h * 2))) return;
        for (int y = y0; y <= y1; y++) {
            float py = y + 0.5f;
            for (int x = x0; x <= x1; x++) {
                float px = x + 0.5f;
                float w0 = ((sx[1] - px) * (sy[2] - py) - (sy[1] - py) * (sx[2] - px)) / area;
                float w1 = ((sx[2] - px) * (sy[0] - py) - (sy[2] - py) * (sx[0] - px)) / area;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                uint16_t z = (uint16_t)((uint32_t)(w0 * sz[0] + w1 * sz[1] + w2 * sz[2]) * 63u) & 0xFFFC;
                uint16_t* p = reinterpret_cast<uint16_t*>(rdram + ((zimg_addr + (uint32_t)(y * fb_w + x) * 2) ^ 2));
                if (depth_test && z >= *p) continue;
                *p = z;
                depth_raster_pixels++;
            }
        }
    }

    // FILL rectangles into small off-screen images are also done in RDRAM (the score buffers'
    // 0xFFFC clears, see depth_raster). Returns true when the target is a z-buffer, which then gets
    // no GPU draw (its write-back would overwrite the depth values).
    bool cpu_fill_rect(float ulx, float uly, float lrx, float lry) {
        int fb_w = (int)cimg_real_width;
        int x0 = std::max(0, std::max((int)ulx, (int)scissor[0]));
        int x1 = std::min(fb_w - 1, std::min((int)lrx, (int)scissor[2] - 1));
        int y0 = std::max(0, std::max((int)uly, (int)scissor[1]));
        int y1 = std::min((int)lry, (int)scissor[3] - 1);
        if (x0 > x1 || y0 > y1 || !in_range(cimg_addr, (uint32_t)((y1 + 1) * fb_w * 2))) return false;
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                uint16_t px = (x & 1) ? (uint16_t)fill_color : (uint16_t)(fill_color >> 16);
                *reinterpret_cast<uint16_t*>(rdram + ((cimg_addr + (uint32_t)(y * fb_w + x) * 2) ^ 2)) = px;
            }
        }
        return depth_images.count(cimg_addr) || cimg_addr == zimg_addr || fill_color == 0xFFFCFFFC;
    }

    // The detection's COPY rectangle into an 8-pixel-wide buffer: write the tile as RGBA5551.
    void center_copy() {
        for (int k = 0; k < 64; k++) {
            uint32_t h = center_id[k] * 2654435761u;
            uint16_t px = (uint16_t)((h >> 16) | 1);
            uint32_t addr = (cimg_addr + k * 2) & 0x00FFFFFF;
            if (in_range(addr, 2)) *reinterpret_cast<uint16_t*>(rdram + (addr ^ 2)) = px;
        }
    }

    void add_depth_clear() {
        center_reset();
        depth_clear_pending = false;
        DrawCmd* d = begin_draw(0, true);
        d->depth_clear = true;
        d->depth_test = false;
        float side = n64_widescreen && !cimg_offscreen ? (top_w / (top_h / (cimg_width * 0.75f)) - cimg_width) * 0.5f : 0.0f;
        add_rect_vertices(d, -side, 0, (float)cimg_width + side, cimg_width * 0.75f, 0, 0, 0, 0, false);
        mark_state_changed();
    }

    // A COPY-mode texture rectangle into an off-screen image, 1:1 and 16-bit: done on the CPU in
    // RDRAM, as the RDP would, instead of a GPU pass. Pokemon Snap renders each photo's thumbnail
    // into a 64x48 buffer and copies it into a 64x624 store, then cuts 64x24 strips out of the store;
    // the store is taller than the GPU's off-screen area. Returns false to draw it normally.
    bool cpu_copy_rect(int tile_index, float ulx, float uly, float lrx, float lry, float s0, float t0, float dsdx, float dtdy) {
        const Tile& tl = tiles[tile_index & 7];
        if (tl.siz != G_IM_SIZ_16b || std::fabs(dsdx - 1.0f) > 0.01f || std::fabs(dtdy - 1.0f) > 0.01f) return false;
        uint16_t load_tmem;
        const TmemLoad* load = find_load(tl.tmem, load_tmem);
        if (!load) return false;
        uint32_t offset = (uint32_t)(tl.tmem - load_tmem) * 8;
        uint32_t stride, start;
        if (load->src_stride) {
            stride = load->src_stride;
            uint32_t tline = load->tmem_line ? load->tmem_line : stride;
            start = load->src + (offset / tline) * stride + (offset % tline);
        } else {
            if (!tl.line) return false;
            stride = tl.line * 8u;
            start = load->src + offset;
        }
        int x0 = std::max<int>((int)ulx, scissor[0]), y0 = std::max<int>((int)uly, scissor[1]);
        int x1 = std::min<int>({ (int)lrx, (int)scissor[2], (int)cimg_real_width }), y1 = std::min<int>((int)lry, scissor[3]);
        if (x0 >= x1 || y0 >= y1) return true;
        int sx = (int)(s0 - tl.uls / 4.0f) + (x0 - (int)ulx), ty = (int)(t0 - tl.ult / 4.0f) + (y0 - (int)uly);
        if (sx < 0 || ty < 0) return false;
        uint32_t src0 = start + ty * stride + sx * 2;
        uint32_t dst0 = cimg_addr + (y0 * cimg_real_width + x0) * 2;
        sync_offscreen_reads(src0, (uint32_t)(y1 - y0) * stride);                    // the source is up to date
        sync_offscreen_reads(dst0, (uint32_t)(y1 - y0) * cimg_real_width * 2);       // and no write-back lands on the copy
        for (int y = 0; y < y1 - y0; y++) {
            uint32_t s = src0 + y * stride, d = dst0 + y * cimg_real_width * 2;
            for (int x = 0; x < x1 - x0; x++) {
                uint32_t sa = (s + x * 2) & 0x00FFFFFF, da = (d + x * 2) & 0x00FFFFFF;
                if (!in_range(sa, 2) || !in_range(da, 2)) continue;
                *reinterpret_cast<uint16_t*>(rdram + (da ^ 2)) = *reinterpret_cast<const uint16_t*>(rdram + (sa ^ 2));
            }
        }
        static int logged;
        if (logged < 8) {
            logged++;
            fprintf(stderr, "[gfx] copy %dx%d from %06X to %06X (width %u)\n", x1 - x0, y1 - y0, (unsigned)src0, (unsigned)dst0,
                (unsigned)cimg_real_width);
        }
        return true;
    }

    // Screen-space rectangle in N64 pixels (fill or texture rectangle).
    void rect(float ulx, float uly, float lrx, float lry, int tile, float s0, float t0, float dsdx, float dtdy, bool textured, bool flip) {
        if (cimg_offscreen && offscreen_state_logs < 400) {
            offscreen_state_logs++;
            const Tile& tl = tiles[tile & 7];
            uint16_t lt = 0;
            const TmemLoad* ld = textured ? find_load(tl.tmem, lt) : nullptr;
            fprintf(stderr, "[offstate] dl %d cimg %06X rect %.0f,%.0f-%.0f,%.0f cyc %u tex %d st %.1f,%.1f d %.2f,%.2f tile siz %u line %u src %06X stride %u\n",
                dl_count, (unsigned)cimg_addr, ulx, uly, lrx, lry, (unsigned)((other_mode_h >> G_CYC_SHIFT) & 3), textured ? 1 : 0, s0, t0,
                dsdx, dtdy, (unsigned)tl.siz, (unsigned)tl.line, ld ? (unsigned)ld->src : 0u, ld ? (unsigned)ld->src_stride : 0u);
        }
        uint32_t cycle = (other_mode_h >> G_CYC_SHIFT) & 3;
        if (cycle == G_CYC_FILL && cimg_real_width <= 64 && (cimg_offscreen || depth_images.count(cimg_addr)) &&
            cpu_fill_rect(ulx, uly, lrx, lry)) return;
        if (cycle == G_CYC_FILL && cimg_addr == zimg_addr && zimg_addr != 0) {
            // Z buffer clear. The color image is the z-buffer right now, so the clear goes into the
            // next color buffer the game selects (G_SETCIMG), whose frame it belongs to.
            depth_clear_pending = true;
            return;
        }
        if (cycle == G_CYC_COPY && cimg_real_width == 8) {
            center_copy(); // Pokemon Snap's focus detection (see center_raster)
            return;
        }
        if (cycle == G_CYC_FILL || cycle == G_CYC_COPY) {
            lrx += 1.0f;
            lry += 1.0f;
        }
        if (cycle == G_CYC_COPY) dsdx /= 4.0f;
        if (cycle == G_CYC_COPY && textured && cimg_offscreen && !flip && cpu_copy_rect(tile, ulx, uly, lrx, lry, s0, t0, dsdx, dtdy)) return;

        // A full-screen fill into a color buffer starts a new N64 frame there: drop whatever was
        // drawn into that buffer before (frames the VI never showed would otherwise pile up).
        if (cycle == G_CYC_FILL && ulx <= 0 && uly <= 0 && lrx >= cimg_width && lry >= cimg_width * 0.75f) {
            if (!frame) select_frame(cimg_addr);
            frame->vertices.clear();
            frame->draws.clear();
            frame->overlay_vertices.clear();
            frame->overlay_draws.clear();
            overlay_source = -1;
            mark_state_changed();
        }

        // Widescreen: a fill across the whole framebuffer (the sky's colour, a clear) covers the
        // side areas too.
        if (cycle == G_CYC_FILL && n64_widescreen && !cimg_offscreen && ulx <= 0 && lrx >= cimg_width) {
            float side = (top_w / (top_h / (cimg_width * 0.75f)) - cimg_width) * 0.5f;
            ulx -= side;
            lrx += side;
        }

        // Widescreen HUD: a rectangle wholly in the top-right corner (the film counter) moves right by
        // the side area's width, into the screen's corner.
        if (n64_hud_wide && !cimg_offscreen && cycle != G_CYC_FILL && ulx >= cimg_width * 0.625f && lry <= cimg_width * 0.1875f) {
            float side = (top_w / (top_h / (cimg_width * 0.75f)) - cimg_width) * 0.5f;
            ulx += side;
            lrx += side;
        }

        rect_prim_w = 0.0f;
        if ((cycle == G_CYC_1CYCLE || cycle == G_CYC_2CYCLE) && !cimg_offscreen && (other_mode_l & 0x4) && (other_mode_l & Z_CMP)) {
            rect_prim_w = rect_primitive_w();
        }

        uint8_t saved_prim[4];
        memcpy(saved_prim, prim, 4);
        if (cycle == G_CYC_FILL) {
            // Fill color is a 16-bit pixel (repeated) for 16-bit framebuffers.
            uint32_t c = rgba5551((uint16_t)(fill_color >> 16));
            prim[0] = c >> 24; prim[1] = (c >> 16) & 0xFF; prim[2] = (c >> 8) & 0xFF; prim[3] = 255;
            textured = false;
        }
        DrawCmd* d;
        if (!rect_state_changed && frame && last_rect.frame == frame && last_rect.draw_count == frame->draws.size() &&
            last_rect.tile == tile && last_rect.cycle == cycle && last_rect.textured == textured &&
            (cycle != G_CYC_FILL || last_rect.fill_color == fill_color)) {
            d = &frame->draws.back();
            uv_map[0] = last_rect.uv[0];
            uv_map[1] = last_rect.uv[1];
        } else {
            d = begin_draw(tile, true);
            if (!textured) {
                d->tex[0].image = nullptr;
                d->tex[1].image = nullptr;
            }
            if (n64_photo_depth && textured && !cimg_offscreen && d->tex[0].image && photo_is_bitmap(d->tex[0].image->src)) {
                d->photo_plane = true;
            }
            last_rect = { frame, frame->draws.size(), tile, cycle, fill_color, textured, { uv_map[0], uv_map[1] } };
            rect_state_changed = false;
        }
        memcpy(prim, saved_prim, 4);

        float s1 = s0 + dsdx * (lrx - ulx);
        float t1 = t0 + dtdy * (lry - uly);
        bool title_text = n64_title_screen && textured && !cimg_offscreen;
        bool copyright_line = title_text && std::fabs(uly - 208.0f) < 0.5f && std::fabs(lry - 218.0f) < 0.5f && ulx > 70.0f;
        bool copyright_first = copyright_line && std::fabs(ulx - 74.0f) < 0.5f;
        if (copyright_line) { // see add_copyright
            ulx += copyright_shift;
            lrx += copyright_shift;
            // The title screen is up (the intro skipped): no DANKEY KONG over it.
            jb_fade.clear();
            dk_step = -1;
        }
        if (title_text && std::fabs(ulx - 71.0f) < 0.5f && std::fabs(uly - 112.0f) < 0.5f) { // JACK and BEANS (intro_logo_step)
            jb_drawn = true;
            jb_alpha = prim[3];
        }
        add_rect_vertices(d, ulx, uly, lrx, lry, s0, t0, s1, t1, flip);
        if (copyright_first) add_copyright(*d);
        // The title screen's logo is 12 tiles, (35,35)-(286,109); after its last one, the subtitle.
        if (n64_title_screen && textured && !cimg_offscreen && std::fabs(ulx - 227.0f) < 0.5f && std::fabs(uly - 99.0f) < 0.5f &&
            std::fabs(lrx - 286.0f) < 0.5f && std::fabs(lry - 109.0f) < 0.5f) {
            add_subtitle(*d);
        }
        if (!textured && !cimg_offscreen) {
            plain_rect = { frame, (size_t)(d - frame->draws.data()), ulx, uly, lrx, lry };
        } else if (d->photo_plane && plain_rect.frame == frame && plain_rect.draw < frame->draws.size()) {
            // A mat a few pixels wider than the photo (the grey border in the Album and Camera Check, where
            // the six are one batch) goes back with it.
            DrawCmd& mat = frame->draws[plain_rect.draw];
            const float margin = 6.0f;
            if (mat.vertex_count <= 6 * 8 && ulx >= plain_rect.ulx && lrx <= plain_rect.lrx && ulx - plain_rect.ulx <= margin &&
                plain_rect.lrx - lrx <= margin && uly >= plain_rect.uly && lry <= plain_rect.lry + 1.0f) {
                mat.photo_plane = true;
            }
        }
        rect_prim_w = 0.0f;
        state_dirty = true;
    }

    // The distance w of the primitive depth (screen z * 32), under the current projection. Whatever
    // the projection slot holds (the perspective alone, or with the camera's view), z and w of a clip
    // space point are related by z = k * w + c, so z/w = k + c / w.
    float rect_primitive_w() const {
        const Mat4& m = projection;
        int row = 0;
        for (int i = 1; i < 3; i++) {
            if (std::fabs(m.m[i][3]) > std::fabs(m.m[row][3])) row = i;
        }
        if (std::fabs(m.m[row][3]) < 1e-6f) return 0.0f; // not a perspective projection
        float k = m.m[row][2] / m.m[row][3];
        float c = m.m[3][2] - k * m.m[3][3];
        float scale = viewport_scale[2] * 4.0f, trans = viewport_trans[2] * 4.0f;
        if (std::fabs(scale) < 1e-6f) return 0.0f;
        float zndc = (prim_depth / 32.0f - trans) / scale;
        float w = std::fabs(zndc - k) > 1e-6f ? c / (zndc - k) : 0.0f;
        return w > 0.0f ? std::min(w, 32000.0f) : 0.0f;
    }

    // The port's own images in the game's frames (tools/make_subtitle.py, make_dankey_assets.py): a
    // texture from RGBA8 rows, and a draw of it at N64 pixels (x0, y0, w, h) - alpha-blended, its
    // alpha times `alpha`, with `base`'s scissor - after whatever the game drew so far, so what the
    // game draws next (a fade, the menus) covers it too.
    static std::shared_ptr<TexImage> port_image(uint64_t key, int w, int h, const uint32_t* rgba) {
        auto image = std::make_shared<TexImage>();
        image->key = key;
        image->width = image->src_w = (uint16_t)w;
        image->height = image->src_h = (uint16_t)h;
        image->texels.resize((size_t)w * h);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) image->texels[tiled_offset(x, y, w, h)] = rgba[y * w + x];
        }
        return image;
    }

    // (u1, v1): the part of the texture to show, or how often to repeat it with TexWrap::Repeat.
    void add_image(const DrawCmd& base, const std::shared_ptr<TexImage>& image, float x0, float y0, float w, float h, uint8_t alpha, bool linear,
                   float u1 = 1.0f, float v1 = 1.0f, TexWrap wrap = TexWrap::Clamp) {
        DrawCmd d = base;
        d.first_vertex = (uint32_t)frame->vertices.size();
        d.vertex_count = 0;
        d.copy_mode = d.two_cycle = d.fill_mode = false;
        d.cycles[0] = d.cycles[1] = CombinerCycle{ { CC_ZERO, CC_ZERO, CC_ZERO, CC_TEX0 }, { CC_TEX0_A, CC_ZERO, CC_PRIM_A, CC_ZERO } };
        d.prim[0] = d.prim[1] = d.prim[2] = 255;
        d.prim[3] = alpha;
        d.blend = BlendMode::Alpha;
        d.alpha_test = true;
        d.alpha_ref = 1;
        d.depth_test = d.depth_write = d.depth_clear = d.decal = false;
        d.photo_plane = false;
        d.overlay_region = 0;
        d.tex[0] = TexBinding{ image, wrap, wrap, linear };
        d.tex[1] = TexBinding{};

        float fb_wf = (float)cimg_width, fb_hf = fb_wf * 0.75f;
        float kx = (top_h * 4.0f / 3.0f / top_w) / (fb_wf * 0.5f);
        auto corner = [&](float px, float py, float u, float v) {
            GpuVertex g{};
            float x = (px - fb_wf * 0.5f) * kx, y = (fb_hf * 0.5f - py) / (fb_hf * 0.5f);
            g.pos[0] = y; // portrait render target, as to_gpu
            g.pos[1] = -x;
            g.pos[2] = 0.0f;
            g.pos[3] = 1.0f;
            g.uv0[0] = g.uv1[0] = u;
            g.uv0[1] = g.uv1[1] = v;
            return g;
        };
        GpuVertex ul = corner(x0, y0, 0, 0), ur = corner(x0 + w, y0, u1, 0), ll = corner(x0, y0 + h, 0, v1), lr = corner(x0 + w, y0 + h, u1, v1);
        for (const GpuVertex& g : { ul, ll, lr, ul, lr, ur }) frame->vertices.push_back(g);
        d.vertex_count = 6;
        frame->draws.push_back(std::move(d));
    }

    // The port's subtitle, "Double Take!!", under the title screen's logo: after the logo's last tile,
    // under "Snap", clear of the TM and of PRESS START / the menu below.
    void add_subtitle(const DrawCmd& logo) {
        static std::shared_ptr<TexImage> image = port_image(0x44424C54414B4521ull /* "DBLTAKE!" */, subtitle_w, subtitle_h, subtitle_rgba);
        constexpr float w = 176.0f;
        add_image(logo, image, 104.0f, 99.0f, w, w * subtitle_h / subtitle_w, 255, true);
    }

    // The title screen's second copyright line ("(c)1999 Nintendo/HAL Laboratory,Inc.", tiles at
    // (74, 208)-(x, 218)) gets "Dankey Kong 2026" after it, in the game's own font, and the whole
    // line is centred again: its tiles move left by this much.
    static constexpr float copyright_shift = -32.0f;
    void add_copyright(const DrawCmd& line) {
        static std::shared_ptr<TexImage> image = port_image(0x444B32303236ull /* "DK2026" */, dankey_copyright_w, dankey_copyright_h, dankey_copyright_rgba);
        // The line's text ends at x 221; its glyphs start one column into the image, four columns
        // later (as between the line's words), and both move with the line.
        add_image(line, image, 221.0f + 4.0f + copyright_shift, 209.0f, (float)dankey_copyright_w, (float)dankey_copyright_h, 255, false);
    }

    // The intro: after JACK and BEANS, the DANKEY KONG logo, fading in and out the same way. Its fade
    // (the primitive alpha of its first tile at (71, 112), one per display list) is recorded and
    // played back for the logo once JACK and BEANS is gone (at each display list's end).
    std::vector<uint8_t> jb_fade;
    bool jb_drawn = false;
    uint8_t jb_alpha = 0;
    int dk_step = -1; // the fade step being played back, -1 = not yet
    void intro_logo_step() {
        if (jb_drawn) {
            if (dk_step >= 0) { // a new showing (the intro plays again)
                jb_fade.clear();
                dk_step = -1;
            }
            jb_fade.push_back(jb_alpha);
            jb_drawn = false;
            return;
        }
        if (jb_fade.empty() || !frame || cimg_offscreen || !n64_title_screen) return;
        if (dk_step < 0) dk_step = 0;
        if (dk_step >= (int)jb_fade.size()) {
            jb_fade.clear();
            dk_step = -1;
            return;
        }
        static std::shared_ptr<TexImage> image = port_image(0x44414E4B4559ull /* "DANKEY" */, dankey_logo_w, dankey_logo_h, dankey_logo_rgba);
        DrawCmd base{};
        base.scissor[2] = (uint16_t)top_w;
        base.scissor[3] = (uint16_t)top_h;
        // Centred where JACK and BEANS was (x 71-249, around y 120), a little wider.
        constexpr float w = 240.0f, h = w * dankey_logo_h / dankey_logo_w;
        add_image(base, image, 160.0f - w / 2, 120.0f - h / 2, w, h, jb_fade[dk_step++], true);
    }

    // Camera Check on the touch screen (camera_check.cpp): while it's in charge, the top screen shows
    // the chosen photo large - the game's 2x render of it (280x210), else its grid render until that
    // comes - over the game's own screen: on Camera Check's background tile (across the whole screen),
    // in its big view's grey frame, with that view's marks (album lower left, Oak lower right) and
    // NEW badge. At each display list's end.
    uint64_t cc_key = 0;
    std::shared_ptr<TexImage> cc_photo;
    int cc_w = 0, cc_h = 0;
    std::vector<uint16_t> cc_pixels;
    static std::shared_ptr<TexImage> solid(uint64_t key, uint32_t rgba) {
        uint32_t px[64];
        for (uint32_t& p : px) p = rgba;
        return port_image(key, 8, 8, px);
    }
    void camera_check_step() {
        if (!frame || cimg_offscreen) return;
        uint64_t hash;
        uint8_t marks;
        if (!cc_top(&hash, &marks)) return;
        static std::shared_ptr<TexImage> tile = port_image(0x4343544C45ull /* "CCTLE" */, cc_tile_w, cc_tile_h, cc_tile_rgba);
        static std::shared_ptr<TexImage> oak = port_image(0x43434F414Bull /* "CCOAK" */, cc_oak_w, cc_oak_h, cc_oak_rgba);
        static std::shared_ptr<TexImage> album = port_image(0x4343414C42ull /* "CCALB" */, cc_album_w, cc_album_h, cc_album_rgba);
        static std::shared_ptr<TexImage> fresh = port_image(0x43434E4557ull /* "CCNEW" */, cc_new_w, cc_new_h, cc_new_rgba);
        static std::shared_ptr<TexImage> grey = solid(0x434347524559ull /* "CCGREY" */, 0xC8C8C8FFu);
        static std::shared_ptr<TexImage> shade = solid(0x4343534844ull /* "CCSHD" */, 0x00000070u);
        static std::shared_ptr<TexImage> black = solid(0x4343424C4Bull /* "CCBLK" */, 0x101010FFu);

        int w = photo_cache_width(hash);
        uint64_t key = (hash ^ 0x4343000000000000ull) + (uint64_t)w;
        if (key != cc_key) {
            cc_key = key;
            cc_photo.reset();
            cc_pixels.resize(320 * 240);
            int pw, ph;
            if (w > 0 && photo_cache_get(hash, &pw, &ph, cc_pixels.data(), 320 * 240) && pw <= 512 && ph <= 256) {
                auto image = std::make_shared<TexImage>();
                image->key = key;
                image->width = 512;
                image->height = 256;
                image->src_w = (uint16_t)pw;
                image->src_h = (uint16_t)ph;
                image->texels.assign(512 * 256, 0x000000FFu);
                for (int y = 0; y < ph; y++) {
                    for (int x = 0; x < pw; x++) {
                        uint16_t p = cc_pixels[(size_t)y * pw + x];
                        uint32_t r = (p >> 11) & 31, g = (p >> 6) & 31, b = (p >> 1) & 31;
                        image->texels[tiled_offset(x, y, 512, 256)] = ((r * 255 / 31) << 24) | ((g * 255 / 31) << 16) | ((b * 255 / 31) << 8) | 0xFF;
                    }
                }
                cc_photo = image;
                cc_w = pw;
                cc_h = ph;
            }
        }

        DrawCmd base{};
        base.scissor[2] = (uint16_t)top_w;
        base.scissor[3] = (uint16_t)top_h;
        float side = (top_w / (top_h / 240.0f) - 320.0f) * 0.5f; // the screen's width past 4:3
        add_image(base, tile, -side, 0, 320 + side * 2, 240, 255, false, (320 + side * 2) / 16.0f, 240 / 16.0f, TexWrap::Repeat);
        constexpr float px = 20, py = 12, pw = 280, ph = 210;
        add_image(base, shade, px + 1, py + 2, pw + 6, ph + 6, 255, false);
        add_image(base, grey, px - 3, py - 3, pw + 6, ph + 6, 255, false);
        if (cc_photo) add_image(base, cc_photo, px, py, pw, ph, 255, true, cc_w / 512.0f, cc_h / 256.0f);
        else add_image(base, black, px, py, pw, ph, 255, false);
        if (marks & CC_ALBUM) add_image(base, album, px + 6, py + ph - 54, 48, 48, 255, false);
        if (marks & CC_OAK) add_image(base, oak, px + pw - 54, py + ph - 54, 48, 48, 255, false);
        if (marks & CC_NEW) add_image(base, fresh, px + pw - 58, py + 6, 64, 32, 255, false);
    }

    void add_rect_vertices(DrawCmd* d, float ulx, float uly, float lrx, float lry, float s0, float t0, float s1, float t1, bool flip) {
        float fb_wf = (float)cimg_width;
        float fb_hf = fb_wf * 0.75f;
        float kx = (top_h * 4.0f / 3.0f / top_w) / (fb_wf * 0.5f);
        auto make = [&](float px, float py, float s, float t) {
            Vertex v;
            if (cimg_offscreen) {
                v.x = px / 256.0f - 1.0f;
                v.y = 1.0f - py / 128.0f;
            } else {
                v.x = (px - fb_wf * 0.5f) * kx;
                v.y = (fb_hf * 0.5f - py) / (fb_hf * 0.5f);
            }
            v.z = 0.0f;
            v.w = 1.0f;
            if (rect_prim_w > 0.0f && !d->depth_clear) {
                // At the primitive depth's distance (the w-buffer's depth is the distance, as for 3D).
                v.x *= rect_prim_w;
                v.y *= rect_prim_w;
                v.z = -0.5f * rect_prim_w;
                v.w = rect_prim_w;
            }
            if (d->depth_clear) {
                // Depth clears go to the far end of the w-buffer (depth -z/32768, c3d_backend.cpp):
                // the same screen position with w = 32768 and z just inside -w.
                constexpr float far_w = 32768.0f;
                v.x *= far_w;
                v.y *= far_w;
                v.z = -0.9999f * far_w;
                v.w = far_w;
            }
            v.u = s;
            v.v = t;
            // Rectangles have no shade on the RDP: a combiner reading SHADE gets zero (the boot logo's
            // background quad uses it to draw black).
            v.color[0] = v.color[1] = v.color[2] = v.color[3] = 0;
            return v;
        };
        Vertex ul = make(ulx, uly, s0, t0);
        Vertex ur = flip ? make(lrx, uly, s0, t1) : make(lrx, uly, s1, t0);
        Vertex ll = flip ? make(ulx, lry, s1, t0) : make(ulx, lry, s0, t1);
        Vertex lr = make(lrx, lry, s1, t1);
        refresh_uv_inv();
        const Vertex* quad[6] = { &ul, &ll, &lr, &ul, &lr, &ur };
        if (int region = hud_region(d, quad, 6)) {
            emit_overlay(quad, 6, d, region);
            return;
        }
        emit(ul, d); emit(ll, d); emit(lr, d);
        emit(ul, d); emit(lr, d); emit(ur, d);
    }

    // --- Interpreter ---

    void run(uint32_t dl_address) {
        Phase phase(PHASE_CMD);
        // Which HUD gauges the game shows (Mega Man 64: life bar status at 0x801BC660 bit 1; energy
        // bar while 0x80204B21 is set and its status at 0x801BC684 has bit 1), so only they move
        // to the bottom screen.
        // Only while the game's HUD routines ran since the last display list (counted by
        // snap_patches.cpp): other screens, like the start menu, draw in the same corners.
        // A pass counts for a few display lists: the game doesn't run its HUD routines for every one
        // (the gauges flickered back onto the top screen).
        static uint32_t last_gauge_passes, last_map_passes, gauge_dl, map_dl;
        uint32_t gauge_passes = snap_hud_gauge_passes, map_passes = snap_hud_map_passes;
        if (gauge_passes != last_gauge_passes) gauge_dl = dl_count;
        if (map_passes != last_map_passes) map_dl = dl_count;
        bool gauges_ran = dl_count - gauge_dl <= 4, map_ran = dl_count - map_dl <= 4;
        last_gauge_passes = gauge_passes;
        last_map_passes = map_passes;
        hud_life_shown = gauges_ran && (read_u8(0x1BC660) & 2);
        hud_energy_shown = gauges_ran && read_u8(0x204B21) != 0 && (read_u8(0x1BC684) & 2);
        // Minimap (func_8004A81C): drawn while 0x801BC441 is clear and 0x800AD0C8 has bit 1.
        hud_map_shown = map_ran && read_u8(0x1BC441) == 0 && (read_u32(0x0AD0C8) & 2);
        uint32_t stack[dl_stack_size];
        int depth = 0;
        uint32_t pc = resolve(dl_address);
        bool survey = dl_count < 3;
        dl_count++;

        // Real display lists are a few thousand commands; anything far beyond that is a runaway
        // (bad branch or missed G_ENDDL). Stop and log it rather than stall the graphics thread.
        constexpr int max_commands = 100000;
        uint32_t recent[8] = {};
        for (int guard = 0;; guard++) {
            if (guard == max_commands) {
                if (runaway_logs < 8) {
                    runaway_logs++;
                    fprintf(stderr, "[gfx] runaway display list 0x%08X (depth %d, pc 0x%06X), last opcodes:",
                        (unsigned)dl_address, depth, (unsigned)pc);
                    for (int i = 0; i < 8; i++) fprintf(stderr, " %08X", (unsigned)recent[(guard + i) & 7]);
                    fprintf(stderr, "\n");
                }
                return;
            }
            uint32_t w0 = read_u32(pc);
            uint32_t w1 = read_u32(pc + 4);
            recent[guard & 7] = w0;
            pc += 8;
            uint8_t op = w0 >> 24;
            if (survey) opcode_counts[op]++;
            if (n64_dltrace_dl == dl_count) { // development (pokes "dltrace N"): every command of display list N
                fprintf(stderr, "[dl] %06X %08X %08X d%d draws%u\n", (unsigned)(pc - 8), (unsigned)w0, (unsigned)w1, depth,
                    frame ? (unsigned)frame->draws.size() : 0u);
            }

            switch (op) {
                case G_NOOP: case G_SPNOOP: case G_RDPLOADSYNC: case G_RDPPIPESYNC: case G_RDPTILESYNC:
                case G_SETPRIMDEPTH:
                    if (prim_depth != ((w1 >> 16) & 0x7FFF)) {
                        prim_depth = (w1 >> 16) & 0x7FFF;
                        mark_state_changed();
                    }
                    break;
                case G_RDPFULLSYNC: case G_SETKEYGB: case G_SETKEYR: case G_SETCONVERT:
                case G_CULLDL: case G_DMA_IO: case G_LOAD_UCODE:
                    break;

                case G_VTX: {
                    int n = (w0 >> 12) & 0xFF;
                    int end = (w0 >> 1) & 0x7F;
                    load_vertices(resolve(w1), n, end - n);
                    break;
                }
                case G_MODIFYVTX: {
                    int idx = ((w0 & 0xFFFF) / 2) & (vertex_buffer_size - 1);
                    int where = (w0 >> 16) & 0xFF;
                    if (where == 0x10) { // RGBA
                        vertices[idx].color[0] = w1 >> 24; vertices[idx].color[1] = w1 >> 16;
                        vertices[idx].color[2] = w1 >> 8; vertices[idx].color[3] = w1;
                    }
                    else if (where == 0x14) { // ST
                        vertices[idx].u = (int16_t)(w1 >> 16) / 32.0f * (texture_scale_s / 65536.0f);
                        vertices[idx].v = (int16_t)(w1 & 0xFFFF) / 32.0f * (texture_scale_t / 65536.0f);
                    }
                    break;
                }
                case G_TRI1:
                    triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                    break;
                case G_TRI2: case G_QUAD:
                    triangle(((w0 >> 16) & 0xFF) / 2, ((w0 >> 8) & 0xFF) / 2, (w0 & 0xFF) / 2);
                    triangle(((w1 >> 16) & 0xFF) / 2, ((w1 >> 8) & 0xFF) / 2, (w1 & 0xFF) / 2);
                    break;
                case G_LINE3D:
                    break; // TODO: L3DEX2 lines
                case G_BRANCH_Z: {
                    // gSPBranchLessZ: jump to the display list in the preceding RDPHALF_1 if the
                    // vertex's screen depth is less than w1 (16.16). Pokemon Snap's levels use it to
                    // pick a terrain chunk's detailed version when it's near.
                    int idx = (int)((w0 & 0xFFF) / 2) & (vertex_buffer_size - 1);
                    if (vertices[idx].screen_z * 65536.0f < (float)w1 && depth < dl_stack_size) pc = resolve(rdphalf_1);
                    break;
                }

                case G_TEXTURE:
                    texture_scale_s = w1 >> 16;
                    texture_scale_t = w1 & 0xFFFF;
                    texture_tile = (w0 >> 8) & 7;
                    texture_on = ((w0 >> 1) & 0x7F) != 0;
                    mark_state_changed();
                    break;
                case G_POPMTX: {
                    int count = w1 / 64;
                    modelview_depth = std::max(0, modelview_depth - count);
                    mvp_dirty = true;
                    break;
                }
                case G_GEOMETRYMODE:
                    geometry_mode = (geometry_mode & (w0 & 0xFFFFFF)) | w1;
                    mark_state_changed();
                    break;
                case G_MTX: {
                    uint8_t p = (w0 & 0xFF) ^ G_MTX_PUSH;
                    Mat4 m = read_matrix(resolve(w1));
                    if (p & G_MTX_PROJECTION) {
                        projection = (p & G_MTX_LOAD) ? m : mul(m, projection);
                    }
                    else {
                        if ((p & G_MTX_PUSH) && modelview_depth < matrix_stack_size - 1) {
                            modelview_stack[modelview_depth + 1] = modelview_stack[modelview_depth];
                            modelview_depth++;
                        }
                        modelview_stack[modelview_depth] = (p & G_MTX_LOAD) ? m : mul(m, modelview_stack[modelview_depth]);
                    }
                    mvp_dirty = true;
                    break;
                }
                case 0xD5: // G_SPECIAL_1: F3DEX2 recomputes the combined matrix (gSPMvpRecalc)
                    mvp_dirty = true;
                    update_mvp();
                    break;
                case G_MOVEWORD: {
                    uint8_t index = (w0 >> 16) & 0xFF;
                    uint16_t offset = w0 & 0xFFFF;
                    switch (index) {
                        case G_MW_MATRIX: {
                            // Writes two halfwords of the RSP's combined matrix (s15.16): integer
                            // halves at 0x00-0x1F, fraction halves at 0x20-0x3F, row-major. Pokemon
                            // Snap patches the matrix this way after gSPMvpRecalc (billboards).
                            update_mvp();
                            bool frac = offset >= 0x20;
                            uint32_t first = ((offset & 0x1F) / 2) & 0xE;
                            for (int k = 0; k < 2; k++) {
                                uint32_t e = first + k;
                                float& m = mvp.m[e / 4][e % 4];
                                int32_t fixed = (int32_t)std::lround(m * 65536.0f);
                                uint16_t half = (uint16_t)(k == 0 ? w1 >> 16 : w1);
                                if (frac) fixed = (fixed & ~0xFFFF) | half;
                                else fixed = (int32_t)((uint32_t)half << 16 | (fixed & 0xFFFF));
                                m = fixed / 65536.0f;
                            }
                            break;
                        }
                        case G_MW_SEGMENT: segments[(offset / 4) & 0xF] = w1 & 0x00FFFFFF; break;
                        case G_MW_NUMLIGHT: num_lights = std::min<int>(7, w1 / 24); break;
                        case G_MW_FOG: fog_mul = (int16_t)(w1 >> 16); fog_offset = (int16_t)(w1 & 0xFFFF); break;
                        case G_MW_LIGHTCOL: {
                            int l = std::min(7, offset / 24);
                            lights[l].col[0] = (w1 >> 24) & 0xFF; lights[l].col[1] = (w1 >> 16) & 0xFF; lights[l].col[2] = (w1 >> 8) & 0xFF;
                            break;
                        }
                        default: break;
                    }
                    break;
                }
                case G_MOVEMEM: {
                    uint8_t index = w0 & 0xFF;
                    uint32_t offset = ((w0 >> 8) & 0xFF) * 8;
                    uint32_t src = resolve(w1);
                    if (index == G_MV_VIEWPORT) {
                        for (int i = 0; i < 3; i++) {
                            viewport_scale[i] = (int16_t)read_u16(src + i * 2) / 4.0f;
                            viewport_trans[i] = (int16_t)read_u16(src + 8 + i * 2) / 4.0f;
                        }
                        if (cimg_offscreen && offscreen_state_logs < 200) {
                            offscreen_state_logs++;
                            fprintf(stderr, "[offstate] dl %d cimg %06X viewport scale %.1f,%.1f trans %.1f,%.1f\n", dl_count,
                                (unsigned)cimg_addr, viewport_scale[0], viewport_scale[1], viewport_trans[0], viewport_trans[1]);
                        }
                    }
                    else if (index == G_MV_LIGHT) {
                        int l = (int)(offset / 24) - 2;
                        float col[3] = { (float)read_u8(src), (float)read_u8(src + 1), (float)read_u8(src + 2) };
                        float dir[3] = { (float)(int8_t)read_u8(src + 8), (float)(int8_t)read_u8(src + 9), (float)(int8_t)read_u8(src + 10) };
                        if (l < 0) {
                            int which = (int)(offset / 24) & 1;
                            memcpy(lookat[which], dir, sizeof(dir));
                        }
                        else if (l < 8) {
                            memcpy(lights[l].col, col, sizeof(col));
                            memcpy(lights[l].dir, dir, sizeof(dir));
                        }
                    }
                    else if (index == G_MV_MATRIX) {
                        // Forced MVP matrix: load into projection with identity modelview.
                        projection = read_matrix(src);
                        modelview_stack[modelview_depth] = identity();
                        mvp_dirty = true;
                    }
                    break;
                }
                case G_DL:
                    if (((w0 >> 16) & 0xFF) == 0) {
                        if (depth >= dl_stack_size) break;
                        stack[depth++] = pc;
                    }
                    pc = resolve(w1);
                    break;
                case G_ENDDL:
                    if (depth == 0) {
                        intro_logo_step();
                        camera_check_step();
                        return;
                    }
                    pc = stack[--depth];
                    break;
                case G_RDPHALF_1: rdphalf_1 = w1; break;
                case G_RDPHALF_2: rdphalf_2 = w1; break;
                case G_SETOTHERMODE_L: case G_SETOTHERMODE_H: {
                    uint32_t len = (w0 & 0xFF) + 1;
                    uint32_t shift = 32 - ((w0 >> 8) & 0xFF) - len;
                    uint32_t mask = (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1)) << shift;
                    uint32_t& om = op == G_SETOTHERMODE_L ? other_mode_l : other_mode_h;
                    uint32_t v = (om & ~mask) | (w1 & mask);
                    if (v != om) {
                        om = v;
                        mark_state_changed();
                    }
                    break;
                }
                case G_RDPSETOTHERMODE:
                    if (other_mode_h != (w0 & 0xFFFFFF) || other_mode_l != w1) {
                        other_mode_h = w0 & 0xFFFFFF;
                        other_mode_l = w1;
                        mark_state_changed();
                    }
                    break;

                case G_TEXRECT: case G_TEXRECTFLIP: {
                    // Followed by RDPHALF_1 (s, t) and RDPHALF_2 (dsdx, dtdy).
                    uint32_t h1 = read_u32(pc + 4);
                    uint32_t h2 = read_u32(pc + 12);
                    pc += 16;
                    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
                    int tile = (w1 >> 24) & 7;
                    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
                    float s = (int16_t)(h1 >> 16) / 32.0f, t = (int16_t)(h1 & 0xFFFF) / 32.0f;
                    float dsdx = (int16_t)(h2 >> 16) / 1024.0f, dtdy = (int16_t)(h2 & 0xFFFF) / 1024.0f;
                    UvMap saved[2] = { uv_map[0], uv_map[1] };
                    rect(ulx, uly, lrx, lry, tile, s, t, dsdx, dtdy, true, op == G_TEXRECTFLIP);
                    uv_map[0] = saved[0];
                    uv_map[1] = saved[1];
                    break;
                }
                case G_FILLRECT: {
                    float lrx = ((w0 >> 12) & 0xFFF) / 4.0f, lry = (w0 & 0xFFF) / 4.0f;
                    float ulx = ((w1 >> 12) & 0xFFF) / 4.0f, uly = (w1 & 0xFFF) / 4.0f;
                    rect(ulx, uly, lrx, lry, 0, 0, 0, 0, 0, false, false);
                    break;
                }
                case G_SETSCISSOR:
                    scissor[0] = ((w0 >> 12) & 0xFFF) / 4; scissor[1] = (w0 & 0xFFF) / 4;
                    scissor[2] = ((w1 >> 12) & 0xFFF) / 4; scissor[3] = (w1 & 0xFFF) / 4;
                    mark_state_changed();
                    if (cimg_offscreen && offscreen_state_logs < 200) {
                        offscreen_state_logs++;
                        fprintf(stderr, "[offstate] dl %d cimg %06X scissor %u,%u,%u,%u\n", dl_count, (unsigned)cimg_addr,
                            scissor[0], scissor[1], scissor[2], scissor[3]);
                    }
                    break;
                case G_SETTILE: {
                    Tile& old = tiles[(w1 >> 24) & 7];
                    Tile t = old;
                    t.fmt = (w0 >> 21) & 7; t.siz = (w0 >> 19) & 3; t.line = (w0 >> 9) & 0x1FF; t.tmem = w0 & 0x1FF;
                    t.palette = (w1 >> 20) & 0xF; t.cmt = (w1 >> 18) & 3; t.maskt = (w1 >> 14) & 0xF; t.shiftt = (w1 >> 10) & 0xF;
                    t.cms = (w1 >> 8) & 3; t.masks = (w1 >> 4) & 0xF; t.shifts = w1 & 0xF;
                    if (memcmp(&t, &old, sizeof(Tile)) != 0) {
                        old = t;
                        mark_state_changed();
                    }
                    break;
                }
                case G_SETTILESIZE: {
                    Tile& old = tiles[(w1 >> 24) & 7];
                    Tile t = old;
                    t.uls = (w0 >> 12) & 0xFFF; t.ult = w0 & 0xFFF; t.lrs = (w1 >> 12) & 0xFFF; t.lrt = w1 & 0xFFF;
                    if (memcmp(&t, &old, sizeof(Tile)) != 0) {
                        old = t;
                        mark_state_changed();
                    }
                    break;
                }
                case G_LOADBLOCK: {
                    Tile& t = tiles[(w1 >> 24) & 7];
                    uint32_t uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF;
                    uint32_t bpp2 = bytes_per_texel_x2(timg_siz);
                    TmemLoad l;
                    l.src = timg_addr + ((ult * timg_width + uls) * bpp2) / 2;
                    l.odd_rows_swapped = (w1 & 0xFFF) == 0; // dxt
                    set_tmem_load(t.tmem, l);
                    break;
                }
                case G_LOADTILE: {
                    Tile& t = tiles[(w1 >> 24) & 7];
                    uint32_t uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2;
                    uint32_t bpp2 = bytes_per_texel_x2(timg_siz);
                    TmemLoad l;
                    l.src_stride = (timg_width * bpp2) / 2;
                    l.src = timg_addr + ult * l.src_stride + (uls * bpp2) / 2;
                    l.tmem_line = t.line * 8;
                    set_tmem_load(t.tmem, l);
                    break;
                }
                case G_LOADTLUT: {
                    Tile& t = tiles[(w1 >> 24) & 7];
                    uint32_t count = ((w1 >> 14) & 0x3FF) + 1;
                    uint32_t base = t.tmem >= 256 ? t.tmem - 256 : 0;
                    last_tlut_base = base; last_tlut_count = count; last_tlut_src = timg_addr;
                    for (uint32_t i = 0; i < count && base + i < 256; i++) {
                        uint32_t a = timg_addr + i * 2;
                        palette[base + i] = (uint16_t)((read_u8(a) << 8) | read_u8(a + 1));
                    }
                    palette_hash = hash_bytes(0, reinterpret_cast<const uint8_t*>(palette), sizeof(palette));
                    mark_state_changed();
                    break;
                }
                case G_SETFILLCOLOR: fill_color = w1; break;
                case G_SETFOGCOLOR: fog_color[0] = w1 >> 24; fog_color[1] = w1 >> 16; fog_color[2] = w1 >> 8; fog_color[3] = w1; mark_state_changed(); break;
                case G_SETBLENDCOLOR: blend_color[0] = w1 >> 24; blend_color[1] = w1 >> 16; blend_color[2] = w1 >> 8; blend_color[3] = w1; mark_state_changed(); break;
                case G_SETPRIMCOLOR: {
                    uint8_t c[4] = { (uint8_t)(w1 >> 24), (uint8_t)(w1 >> 16), (uint8_t)(w1 >> 8), (uint8_t)w1 };
                    if (memcmp(c, prim, 4) != 0 || prim_lod_frac != (w0 & 0xFF)) {
                        prim_lod_frac = w0 & 0xFF;
                        memcpy(prim, c, 4);
                        mark_state_changed();
                    }
                    break;
                }
                case G_SETENVCOLOR: {
                    uint8_t c[4] = { (uint8_t)(w1 >> 24), (uint8_t)(w1 >> 16), (uint8_t)(w1 >> 8), (uint8_t)w1 };
                    if (memcmp(c, env, 4) != 0) {
                        memcpy(env, c, 4);
                        mark_state_changed();
                    }
                    break;
                }
                case G_SETCOMBINE:
                    if (combine_w0 != (w0 & 0xFFFFFF) || combine_w1 != w1) {
                        combine_w0 = w0 & 0xFFFFFF;
                        combine_w1 = w1;
                        mark_state_changed();
                    }
                    break;
                case G_SETTIMG:
                    timg_addr = resolve(w1);
                    timg_siz = (w0 >> 19) & 3;
                    timg_width = (w0 & 0xFFF) + 1;
                    break;
                case G_SETZIMG: zimg_addr = resolve(w1); depth_images.insert(zimg_addr); sync_candidates_valid = false; break;
                case G_SETCIMG: {
                    cimg_addr = resolve(w1);
                    cimg_width = (w0 & 0xFFF) + 1;
                    cimg_real_width = cimg_width;
                    if (cimg_width < 64 || cimg_width > 1024) cimg_width = 320;
                    select_frame(cimg_addr);
                    if (frame->vertices.size() > max_frame_vertices) {
                        frame->vertices.clear();
                        frame->draws.clear();
                        frame->overlay_vertices.clear();
                        frame->overlay_draws.clear();
                        overlay_source = -1;
                    }
                    cimg_offscreen = cimg_real_width >= 64 && vi_origins.size() >= 2 && !shown_by_vi(cimg_addr) &&
                                     !depth_images.count(cimg_addr);
                    if (cimg_offscreen && offscreen_state_logs < 400) {
                        offscreen_state_logs++;
                        fprintf(stderr, "[offstate] dl %d cimg %06X width %u fmt %u siz %u\n", dl_count, (unsigned)cimg_addr,
                            (unsigned)cimg_real_width, (unsigned)((w0 >> 21) & 7), (unsigned)((w0 >> 19) & 3));
                    }
                    if (depth_clear_pending && cimg_addr != zimg_addr) add_depth_clear();
                    break;
                }

                default:
                    if (logged_unknown.size() < 32 && logged_unknown.insert(op).second) {
                        fprintf(stderr, "[gfx] unhandled opcode %02X (w0=%08X w1=%08X)\n", op, (unsigned)w0, (unsigned)w1);
                    }
                    break;
            }

        }
    }

    void write_survey() {
        fprintf(stderr, "[gfx] opcode survey (first 3 display lists):");
        for (int i = 0; i < 256; i++) {
            if (opcode_counts[i]) fprintf(stderr, " %02X:%u", i, (unsigned)opcode_counts[i]);
        }
        fprintf(stderr, "\n[gfx] color image 0x%06X width %u, z image 0x%06X\n", (unsigned)cimg_addr, (unsigned)cimg_width, (unsigned)zimg_addr);
    }
};

// Development: what the renderer holds in memory, for the [mem] log line (main.c watchdog).
static void* mem_report_impl;
void Renderer::mem_report() {
    Impl* r = static_cast<Impl*>(mem_report_impl);
    if (!r) return;
    size_t frame_vtx = 0, frame_draws = 0;
    for (auto& f : r->frames) {
        frame_vtx += f.second->vertices.capacity() + f.second->overlay_vertices.capacity();
        frame_draws += f.second->draws.capacity() + f.second->overlay_draws.capacity();
    }
    size_t pub_vtx = 0;
    for (auto& f : r->last_published) pub_vtx += f.second->vertices.capacity();
    fprintf(stderr, "[mem] renderer: textures %zu (%zu KB) indices %zu (%zu KB) binds %zu sources %zu frames %zu (vtx %zu draws %zu) published %zu (vtx %zu) tmem %zu combiners %zu\n",
        r->texture_cache.size(), r->texture_bytes / 1024, r->index_cache.size(), r->index_bytes / 1024, r->bind_cache.size(),
        r->source_cache.size(), r->frames.size(), frame_vtx, frame_draws, r->last_published.size(), pub_vtx, r->tmem_loads.size(),
        r->logged_combiners.size());
}

Renderer::Renderer(uint8_t* rdram) : impl(std::make_unique<Impl>(rdram)) { mem_report_impl = impl.get(); }
extern "C" void n64_renderer_mem_report(void) { Renderer::mem_report(); }
Renderer::~Renderer() = default;

void Renderer::run_display_list(uint32_t dl_address) {
    impl->run(dl_address);
    if (impl->dl_count == 3) impl->write_survey();
    impl->finish_offscreen_frames();
}

std::shared_ptr<FrameData> Renderer::take_offscreen_frame() {
    std::lock_guard lock{ published_mutex };
    if (offscreen_frames.empty()) return nullptr;
    std::shared_ptr<FrameData> f = std::move(offscreen_frames.front());
    offscreen_frames.pop_front();
    return f;
}

void Renderer::on_vi(uint32_t vi_origin) {
    impl->publish(vi_origin & 0x00FFFFFF);
}

std::shared_ptr<FrameData> Renderer::take_published_frame() {
    std::lock_guard lock{ published_mutex };
    return std::move(published_frame);
}

} // namespace n64r
