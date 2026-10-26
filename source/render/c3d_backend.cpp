// Citro3D backend for the N64 renderer (main thread). Translates n64r::FrameData draw calls into
// PICA200 state: N64 color combiner -> TexEnv stages, render modes -> blend/alpha/depth state.

#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "c3d_backend.h"
#include "n64_frame.hpp"
#include "n64_renderer.hpp"
#include "n64_shbin.h"

// Development (pokes "magenta 1"): clear the top screen to magenta and skip fill rectangles, so
// whatever no polygon covers shows.
extern "C" { volatile bool n64_debug_magenta; }
// Development (pokes "drawid 1"): each draw in a flat color encoding its index in the frame.
extern "C" { volatile bool n64_debug_drawid; }


using namespace n64r;

static C3D_Tex* offscreen_texture(const TexBinding& b); // render-to-texture (below)

namespace {

constexpr u32 display_transfer_flags =
    GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

constexpr size_t vbo_capacity = 65536;
constexpr size_t texture_cache_limit = 1024; // GPU textures kept (a busy area uses ~500 per frame)

DVLB_s* shader_dvlb;
shaderProgram_s program;
C3D_AttrInfo attr_info;
C3D_RenderTarget* top_target;
// Stereoscopic 3D: the right eye's target, the eye separation (0 = no 3D: one image, the screen's 3D
// mode off) and the vertex shader's stereo uniform (n64.v.pica).
static C3D_RenderTarget* top_target_right;
static float stereo_separation;
static int stereo_uniform = -1;
// Photos (DrawCmd::photo_plane): behind the screen by this share of the full separation.
static constexpr float stereo_photo_depth = 0.2f;
GpuVertex* vbo[2];
void* vbo_alloc[2];

// Vertex buffers get pages of their own, with a guard page on each side, so per-frame CPU writes
// never share a 4 KB page with a GPU-written buffer (which makes emulators like Citra read that
// surface back from the host GPU).
constexpr size_t page_size = 0x1000;
constexpr size_t vbo_bytes = (vbo_capacity * sizeof(GpuVertex) + page_size - 1) & ~(page_size - 1);
int vbo_index;
std::shared_ptr<FrameData> current_frame;
C3D_Tex white_tex;
u32 frame_number;
u32 stat_uploads, stat_evictions, stat_draws, stat_reuploads;

struct CachedTex {
    C3D_Tex tex;
    u32 last_used;
    uint64_t image_key; // the TexImage now in it (palette textures change it in place)
};
std::unordered_map<uint64_t, CachedTex*> texture_cache;

// --- Textures -----------------------------------------------------------------------------------

void evict_textures(bool all_unused) {
    for (auto it = texture_cache.begin(); it != texture_cache.end();) {
        // With C3D_FRAME_SYNCDRAW the previous frame has finished on the GPU, so anything not used
        // in this frame can go.
        if (it->second->last_used + (all_unused ? 1 : 2) <= frame_number) {
            C3D_TexDelete(&it->second->tex);
            stat_evictions++;
            delete it->second;
            it = texture_cache.erase(it);
        }
        else {
            ++it;
        }
    }
}

C3D_Tex* get_texture(const TexBinding& b) {
    if (!b.image) return &white_tex;
    if (b.image->rt_address) return offscreen_texture(b);
    // Palette textures share one GPU texture per set of indices (TexImage::gpu_slot): when only the
    // palette changed, its texels are uploaded into it again instead of making a new texture and
    // dropping the old one each frame - unless this frame already drew it with the other palette
    // (the GPU reads textures after the frame is built), in which case it gets a texture of its own.
    uint64_t cache_key = b.image->key;
    if (b.image->gpu_slot) {
        uint64_t slot_key = b.image->gpu_slot ^ 0xA5A5A5A5A5A5A5A5ull;
        auto s = texture_cache.find(slot_key);
        if (s == texture_cache.end()) {
            cache_key = slot_key;
        } else if (s->second->image_key == b.image->key) {
            cache_key = slot_key;
        } else if (s->second->last_used != frame_number && s->second->tex.width == b.image->width &&
                   s->second->tex.height == b.image->height) {
            C3D_TexUpload(&s->second->tex, b.image->texels.data());
            s->second->image_key = b.image->key;
            stat_reuploads++;
            cache_key = slot_key;
        }
    }
    auto it = texture_cache.find(cache_key);
    CachedTex* ct;
    if (it != texture_cache.end()) {
        ct = it->second;
    }
    else {
        if (texture_cache.size() >= texture_cache_limit) evict_textures(false);
        ct = new CachedTex{};
        if (!C3D_TexInit(&ct->tex, b.image->width, b.image->height, GPU_RGBA8)) {
            evict_textures(true);
            if (!C3D_TexInit(&ct->tex, b.image->width, b.image->height, GPU_RGBA8)) {
                delete ct;
                return &white_tex;
            }
        }
        C3D_TexUpload(&ct->tex, b.image->texels.data());
        stat_uploads++;
        ct->image_key = b.image->key;
        texture_cache[cache_key] = ct;
    }
    ct->last_used = frame_number;

    auto wrap = [](TexWrap w) {
        return w == TexWrap::Mirror ? GPU_MIRRORED_REPEAT : w == TexWrap::Repeat ? GPU_REPEAT : GPU_CLAMP_TO_EDGE;
    };
    C3D_TexSetWrap(&ct->tex, wrap(b.wrap_s), wrap(b.wrap_t));
    C3D_TexSetFilter(&ct->tex, b.linear ? GPU_LINEAR : GPU_NEAREST, b.linear ? GPU_LINEAR : GPU_NEAREST);
    return &ct->tex;
}

// --- Combiner -> TexEnv compiler -----------------------------------------------------------------
//
// Each N64 cycle computes (a - b) * c + d per channel. Each PICA200 stage applies one function to up
// to three sources, of which only one may be a constant (the stage's constant color). Equations are
// compiled to 1-3 stages per channel with constant folding; RGB and alpha chains are then padded to
// the same length and zipped. In 2-cycle mode the cycle-1 result is kept in the combiner buffer so
// cycle 2 can read it as COMBINED.

struct Arg {
    bool is_const;
    GPU_TEVSRC src;
    int op;        // GPU_TEVOP_RGB or GPU_TEVOP_A operand
    float v[3];    // constant value (RGB, or v[0] for alpha)
};

struct Stage {
    GPU_COMBINEFUNC func;
    Arg args[3];
    int nargs;
};

Arg make_const(float r, float g, float b) { return Arg{ true, GPU_CONSTANT, 0, { r, g, b } }; }
Arg make_var(GPU_TEVSRC src, int op) { return Arg{ false, src, op, { 0, 0, 0 } }; }

bool same(const Arg& x, const Arg& y) {
    if (x.is_const != y.is_const) return false;
    if (x.is_const) return x.v[0] == y.v[0] && x.v[1] == y.v[1] && x.v[2] == y.v[2];
    return x.src == y.src && x.op == y.op;
}
bool is_value(const Arg& x, float v) { return x.is_const && x.v[0] == v && x.v[1] == v && x.v[2] == v; }

int distinct_consts(std::initializer_list<const Arg*> args) {
    const Arg* first = nullptr;
    int n = 0;
    for (const Arg* a : args) {
        if (!a->is_const) continue;
        if (!first) { first = a; n = 1; }
        else if (!same(*first, *a)) n = 2;
    }
    return n;
}

struct ChannelCompiler {
    bool alpha;
    std::vector<Stage>& out;

    Arg prev() const { return make_var(GPU_PREVIOUS, alpha ? (int)GPU_TEVOP_A_SRC_ALPHA : (int)GPU_TEVOP_RGB_SRC_COLOR); }

    void emit(GPU_COMBINEFUNC f, Arg a0, Arg a1 = Arg{}, Arg a2 = Arg{}, int n = 1) {
        out.push_back(Stage{ f, { a0, a1, a2 }, n });
    }

    void mul(const Arg& x, const Arg& y) {
        if (x.is_const && y.is_const) emit(GPU_REPLACE, make_const(x.v[0] * y.v[0], x.v[1] * y.v[1], x.v[2] * y.v[2]));
        else if (is_value(x, 1)) emit(GPU_REPLACE, y);
        else if (is_value(y, 1)) emit(GPU_REPLACE, x);
        else emit(GPU_MODULATE, x, y, Arg{}, 2);
    }

    void add_prev(const Arg& z) {
        if (!is_value(z, 0)) emit(GPU_ADD, prev(), z, Arg{}, 2);
    }

    // x * y + z
    void mad(const Arg& x, const Arg& y, const Arg& z) {
        if (is_value(z, 0)) { mul(x, y); return; }
        if (distinct_consts({ &x, &y, &z }) <= 1 && !(x.is_const && y.is_const)) {
            emit(GPU_MULTIPLY_ADD, x, y, z, 3);
            return;
        }
        mul(x, y);
        add_prev(z);
    }

    // x * z + y * (1 - z)
    void lerp(const Arg& x, const Arg& y, const Arg& z) {
        if (distinct_consts({ &x, &y, &z }) <= 1) {
            emit(GPU_INTERPOLATE, x, y, z, 3);
            return;
        }
        // z is constant here (x and y both constant was folded by the caller).
        Arg one_minus_z = make_const(1 - z.v[0], 1 - z.v[1], 1 - z.v[2]);
        if (x.is_const) {
            mul(y, one_minus_z);
            add_prev(make_const(x.v[0] * z.v[0], x.v[1] * z.v[1], x.v[2] * z.v[2]));
        }
        else {
            mul(x, z);
            add_prev(make_const(y.v[0] * one_minus_z.v[0], y.v[1] * one_minus_z.v[1], y.v[2] * one_minus_z.v[2]));
        }
    }

    // (a - b) * c + d
    void compile(const Arg& a, const Arg& b, const Arg& c, const Arg& d) {
        if (is_value(c, 0) || same(a, b)) { emit(GPU_REPLACE, d); return; }
        if (a.is_const && b.is_const && c.is_const && d.is_const) {
            float r[3];
            for (int i = 0; i < 3; i++) r[i] = std::clamp((a.v[i] - b.v[i]) * c.v[i] + d.v[i], 0.0f, 1.0f);
            emit(GPU_REPLACE, make_const(r[0], r[1], r[2]));
            return;
        }
        if (a.is_const && b.is_const) {
            float k[3] = { a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2] };
            if (k[0] >= 0 && k[1] >= 0 && k[2] >= 0) {
                mad(c, make_const(k[0], k[1], k[2]), d);
            }
            else {
                // d - |k| * c
                mul(c, make_const(std::max(0.0f, -k[0]), std::max(0.0f, -k[1]), std::max(0.0f, -k[2])));
                emit(GPU_SUBTRACT, d, prev(), Arg{}, 2);
            }
            return;
        }
        if (is_value(b, 0)) { mad(a, c, d); return; }
        if (same(d, b)) { lerp(a, b, c); return; }
        // General case: the PICA200 clamps the subtraction at 0 (N64 keeps it signed).
        emit(GPU_SUBTRACT, a, b, Arg{}, 2);
        if (is_value(c, 1)) add_prev(d);
        else mad(prev(), c, d);
    }
};

// Where the second cycle reads COMBINED (the first cycle's result) from.
enum class CombinedSource { None, Previous, Buffer };

Arg input_arg(CcInput in, bool alpha, const DrawCmd& d, CombinedSource combined) {
    auto col = [](const uint8_t* c) { return make_const(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f); };
    auto splat = [](float v) { return make_const(v, v, v); };
    int color_op = alpha ? (int)GPU_TEVOP_A_SRC_ALPHA : (int)GPU_TEVOP_RGB_SRC_COLOR;
    int alpha_op = alpha ? (int)GPU_TEVOP_A_SRC_ALPHA : (int)GPU_TEVOP_RGB_SRC_ALPHA;
    GPU_TEVSRC combined_src = combined == CombinedSource::Buffer ? GPU_PREVIOUS_BUFFER : GPU_PREVIOUS;
    switch (in) {
        case CC_ZERO: return splat(0);
        case CC_ONE: return splat(1);
        case CC_COMBINED: return combined != CombinedSource::None ? make_var(combined_src, color_op) : splat(0);
        case CC_COMBINED_A: return combined != CombinedSource::None ? make_var(combined_src, alpha_op) : splat(0);
        case CC_TEX0: return make_var(GPU_TEXTURE0, color_op);
        case CC_TEX0_A: return make_var(GPU_TEXTURE0, alpha_op);
        case CC_TEX1: return make_var(GPU_TEXTURE1, color_op);
        case CC_TEX1_A: return make_var(GPU_TEXTURE1, alpha_op);
        case CC_SHADE: return make_var(GPU_PRIMARY_COLOR, color_op);
        case CC_SHADE_A: return make_var(GPU_PRIMARY_COLOR, alpha_op);
        case CC_PRIM: return alpha ? splat(d.prim[3] / 255.0f) : col(d.prim);
        case CC_PRIM_A: return splat(d.prim[3] / 255.0f);
        case CC_ENV: return alpha ? splat(d.env[3] / 255.0f) : col(d.env);
        case CC_ENV_A: return splat(d.env[3] / 255.0f);
        case CC_PRIM_LOD_FRAC: return splat(d.prim_lod_frac / 255.0f);
        // With no mip levels every pixel counts as "distant" on the RDP, which sets the LOD fraction
        // to 0xFF. As 0 it made the opening cutscene's crystal on the pedestal fully transparent.
        case CC_LOD_FRAC: return splat(1);
        case CC_NOISE: return splat(0.5f);
        case CC_SCALE: return splat(1);
        default: return splat(0); // K4, K5, CENTER
    }
}

void compile_cycle(const DrawCmd& d, const CombinerCycle& cc, CombinedSource second, std::vector<Stage>& rgb, std::vector<Stage>& alpha) {
    ChannelCompiler crgb{ false, rgb };
    ChannelCompiler ca{ true, alpha };
    crgb.compile(input_arg(cc.rgb[0], false, d, second), input_arg(cc.rgb[1], false, d, second),
                 input_arg(cc.rgb[2], false, d, second), input_arg(cc.rgb[3], false, d, second));
    ca.compile(input_arg(cc.alpha[0], true, d, second), input_arg(cc.alpha[1], true, d, second),
               input_arg(cc.alpha[2], true, d, second), input_arg(cc.alpha[3], true, d, second));
    // Pad the shorter chain so both finish on the same stage.
    Stage pass_rgb{ GPU_REPLACE, { make_var(GPU_PREVIOUS, GPU_TEVOP_RGB_SRC_COLOR) }, 1 };
    Stage pass_a{ GPU_REPLACE, { make_var(GPU_PREVIOUS, GPU_TEVOP_A_SRC_ALPHA) }, 1 };
    while (rgb.size() < alpha.size()) rgb.push_back(pass_rgb);
    while (alpha.size() < rgb.size()) alpha.push_back(pass_a);
}

u8 to_u8(float v) { return (u8)std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f); }

void apply_stage(int id, const Stage* rgb, const Stage* a) {
    C3D_TexEnv* env = C3D_GetTexEnv(id);
    C3D_TexEnvInit(env);
    if (!rgb) return; // pass-through
    u8 color[4] = { 0, 0, 0, 0 };
    GPU_TEVSRC rs[3], as[3];
    int ro[3], ao[3];
    for (int i = 0; i < 3; i++) {
        const Arg& r = rgb->args[i];
        const Arg& al = a->args[i];
        rs[i] = (i < rgb->nargs) ? r.src : GPU_PREVIOUS;
        ro[i] = (i < rgb->nargs) ? r.op : GPU_TEVOP_RGB_SRC_COLOR;
        if (i < rgb->nargs && r.is_const) {
            color[0] = to_u8(r.v[0]); color[1] = to_u8(r.v[1]); color[2] = to_u8(r.v[2]);
            ro[i] = GPU_TEVOP_RGB_SRC_COLOR;
        }
        as[i] = (i < a->nargs) ? al.src : GPU_PREVIOUS;
        ao[i] = (i < a->nargs) ? al.op : GPU_TEVOP_A_SRC_ALPHA;
        if (i < a->nargs && al.is_const) {
            color[3] = to_u8(al.v[0]);
            ao[i] = GPU_TEVOP_A_SRC_ALPHA;
        }
    }
    C3D_TexEnvSrc(env, C3D_RGB, rs[0], rs[1], rs[2]);
    C3D_TexEnvSrc(env, C3D_Alpha, as[0], as[1], as[2]);
    C3D_TexEnvOpRgb(env, (GPU_TEVOP_RGB)ro[0], (GPU_TEVOP_RGB)ro[1], (GPU_TEVOP_RGB)ro[2]);
    C3D_TexEnvOpAlpha(env, (GPU_TEVOP_A)ao[0], (GPU_TEVOP_A)ao[1], (GPU_TEVOP_A)ao[2]);
    C3D_TexEnvFunc(env, C3D_RGB, rgb->func);
    C3D_TexEnvFunc(env, C3D_Alpha, a->func);
    C3D_TexEnvColor(env, color[0] | (color[1] << 8) | (color[2] << 16) | ((u32)color[3] << 24));
}

void setup_combiner(const DrawCmd& d) {
    std::vector<Stage> rgb, alpha;
    rgb.reserve(6);
    alpha.reserve(6);
    int buffer_stage = -1;

    if (d.fill_mode) {
        Stage s{ GPU_REPLACE, { make_const(d.prim[0] / 255.0f, d.prim[1] / 255.0f, d.prim[2] / 255.0f) }, 1 };
        Stage sa{ GPU_REPLACE, { make_const(1, 1, 1) }, 1 };
        rgb.push_back(s);
        alpha.push_back(sa);
    }
    else if (d.copy_mode) {
        rgb.push_back(Stage{ GPU_REPLACE, { make_var(GPU_TEXTURE0, GPU_TEVOP_RGB_SRC_COLOR) }, 1 });
        alpha.push_back(Stage{ GPU_REPLACE, { make_var(GPU_TEXTURE0, GPU_TEVOP_A_SRC_ALPHA) }, 1 });
    }
    else {
        compile_cycle(d, d.cycles[0], CombinedSource::None, rgb, alpha);
        if (d.two_cycle) {
            // Both channel chains of cycle 1 end on the same stage, so at the first stage of cycle 2
            // PREVIOUS *is* COMBINED. Use that when cycle 2 fits in one stage (the common case).
            size_t cycle1_len = rgb.size();
            std::vector<Stage> rgb2 = rgb, alpha2 = alpha;
            compile_cycle(d, d.cycles[1], CombinedSource::Previous, rgb2, alpha2);
            if (rgb2.size() - cycle1_len <= 1) {
                rgb = std::move(rgb2);
                alpha = std::move(alpha2);
            }
            else {
                // Otherwise keep cycle 1's result in the combiner buffer: its last stage (0-3) writes
                // it, and the buffer only gets there one stage later - stage k reads what stage k - 2
                // wrote - so a pass-through stage goes in between. (Reading it one stage early gave
                // cycle 1's next-to-last result: Mew's bubble in the intro, alpha before the texture
                // was applied, drew as an opaque box.)
                buffer_stage = (int)rgb.size() - 1;
                rgb.push_back(Stage{ GPU_REPLACE, { make_var(GPU_PREVIOUS, GPU_TEVOP_RGB_SRC_COLOR) }, 1 });
                alpha.push_back(Stage{ GPU_REPLACE, { make_var(GPU_PREVIOUS, GPU_TEVOP_A_SRC_ALPHA) }, 1 });
                compile_cycle(d, d.cycles[1], CombinedSource::Buffer, rgb, alpha);
            }
        }
    }

    int n = std::min<int>(6, (int)rgb.size());
    for (int i = 0; i < 6; i++) {
        apply_stage(i, i < n ? &rgb[i] : nullptr, i < n ? &alpha[i] : nullptr);
    }
    // Buffer-update bit k (register bits 8-11) makes stage k (0-3) write its output to the buffer.
    int mask = (buffer_stage >= 0 && buffer_stage <= 3) ? (1 << buffer_stage) : 0;
    C3D_TexEnvBufUpdate(C3D_Both, mask);
    C3D_TexEnvBufColor(0);
}

// Redundant-state elimination: N64 frames often have hundreds of draws that differ only by texture
// (e.g. an image drawn as 32x32 tiles). Re-emitting every register per draw overflows the GPU
// command buffer and slows emulators down, so each state group is only sent when it changes.
// Reset at the start of every frame because citro2d changes the same state for the bottom screen.
struct StateCache {
    bool combiner_valid;
    bool two_cycle, copy_mode, fill_mode;
    CombinerCycle cycles[2];
    uint8_t prim[4], env[4], prim_lod_frac;
    C3D_Tex* tex[2];
    int blend_alpha_key;  // -1 = unknown
    int depth_key;        // -1 = unknown
    u64 scissor_key;
    bool scissor_valid;
} state_cache;

void reset_state_cache() {
    state_cache = StateCache{};
    state_cache.blend_alpha_key = -1;
    state_cache.depth_key = -1;
}

bool same_combiner(const DrawCmd& d) {
    const StateCache& c = state_cache;
    return c.combiner_valid && c.two_cycle == d.two_cycle && c.copy_mode == d.copy_mode && c.fill_mode == d.fill_mode &&
        !memcmp(c.cycles, d.cycles, sizeof(c.cycles)) && !memcmp(c.prim, d.prim, 4) && !memcmp(c.env, d.env, 4) &&
        c.prim_lod_frac == d.prim_lod_frac;
}

void setup_draw_state(const DrawCmd& d_in) {
    StateCache& c = state_cache;
#ifdef N64_AB_TEX_ONLY
    // A/B experiment: 3D draws output TEXEL0 only (ignores shade).
    DrawCmd d = d_in;
    if (!d.copy_mode && !d.fill_mode && (d.geometry_mode & 0x4)) {
        for (auto& cyc : d.cycles) {
            cyc.rgb[0] = cyc.rgb[1] = cyc.rgb[2] = CC_ZERO; cyc.rgb[3] = CC_TEX0;
            cyc.alpha[0] = cyc.alpha[1] = cyc.alpha[2] = CC_ZERO; cyc.alpha[3] = CC_ONE;
        }
        d.two_cycle = false;
    }
#else
    const DrawCmd& d = d_in;
#endif
    if (!same_combiner(d)) {
        setup_combiner(d);
        c.combiner_valid = true;
        c.two_cycle = d.two_cycle; c.copy_mode = d.copy_mode; c.fill_mode = d.fill_mode;
        memcpy(c.cycles, d.cycles, sizeof(c.cycles));
        memcpy(c.prim, d.prim, 4); memcpy(c.env, d.env, 4);
        c.prim_lod_frac = d.prim_lod_frac;
    }

    // get_texture() also applies the binding's wrap/filter to the texture object, so a texture
    // whose parameters changed is re-bound as well.
    for (int unit = 0; unit < 2; unit++) {
        C3D_Tex* t = (unit == 0 || d.tex[1].image) ? get_texture(d.tex[unit]) : &white_tex;
        u32 param = t->param;
        static u32 bound_param[2];
        if (t != c.tex[unit] || param != bound_param[unit]) {
            C3D_TexBind(unit, t);
            c.tex[unit] = t;
            bound_param[unit] = param;
        }
    }

    int blend_alpha_key = (int)d.blend | ((d.alpha_test && d.alpha_ref > 0) ? 4 : 0) | (d.alpha_ref << 3) | (d.blend_alpha << 11);
    if (blend_alpha_key != c.blend_alpha_key) {
        switch (d.blend) {
            case BlendMode::Alpha:
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
                break;
            case BlendMode::ConstantAlpha:
                C3D_BlendingColor((u32)d.blend_alpha << 24);
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA, GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA);
                break;
            case BlendMode::KeepMemory:
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ZERO, GPU_ONE, GPU_ZERO, GPU_ONE);
                break;
            default:
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
                break;
        }
        C3D_AlphaTest(d.alpha_test && d.alpha_ref > 0, GPU_GEQUAL, d.alpha_ref);
        c.blend_alpha_key = blend_alpha_key;
    }

    // Scissor: DrawCmd holds landscape top-screen pixels (x 0..400 right, y 0..240 down). The render
    // target is the portrait 240x400 buffer, where landscape (x, y) lands at (240 - y, 400 - x).
    u64 scissor_key = d.scissor[0] | ((u64)d.scissor[1] << 16) | ((u64)d.scissor[2] << 32) | ((u64)d.scissor[3] << 48);
    scissor_key ^= d.offscreen_map ? 0x8000800080008000ull : 0;
    if (scissor_key != c.scissor_key || !c.scissor_valid) {
        if (d.offscreen_map) // N64 pixels 1:1 in the off-screen target's 512x256 area (portrait 256x512)
            C3D_SetScissor(GPU_SCISSOR_NORMAL, 256 - d.scissor[3], 512 - d.scissor[2], 256 - d.scissor[1], 512 - d.scissor[0]);
        else
            C3D_SetScissor(GPU_SCISSOR_NORMAL, 240 - d.scissor[3], 400 - d.scissor[2], 240 - d.scissor[1], 400 - d.scissor[0]);
        c.scissor_key = scissor_key;
        c.scissor_valid = true;
    }

    int depth_key = d.depth_clear ? 0 : d.depth_test ? (d.depth_write ? 1 : 2) : d.depth_write ? 3 : 4;
    if (depth_key != c.depth_key) {
        switch (depth_key) {
            case 0: C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_DEPTH); break;
            case 1: C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL); break;
            case 2: C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_COLOR); break;
            case 3: C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL); break;
            default: C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR); break;
        }
        c.depth_key = depth_key;
    }
}

} // namespace

extern "C" void n64_top_init(void) {
    top_target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(top_target, GFX_TOP, GFX_LEFT, display_transfer_flags);
    top_target_right = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(top_target_right, GFX_TOP, GFX_RIGHT, display_transfer_flags);

    shader_dvlb = DVLB_ParseFile((u32*)n64_shbin, n64_shbin_size);
    shaderProgramInit(&program);
    shaderProgramSetVsh(&program, &shader_dvlb->DVLE[0]);
    stereo_uniform = shaderInstanceGetUniformLocation(program.vertexShader, "stereo");

    AttrInfo_Init(&attr_info);
    AttrInfo_AddLoader(&attr_info, 0, GPU_FLOAT, 4);         // position
    AttrInfo_AddLoader(&attr_info, 1, GPU_FLOAT, 2);         // uv0
    AttrInfo_AddLoader(&attr_info, 2, GPU_FLOAT, 2);         // uv1
    AttrInfo_AddLoader(&attr_info, 3, GPU_UNSIGNED_BYTE, 4); // color

    for (int i = 0; i < 2; i++) {
        vbo_alloc[i] = linearMemAlign(vbo_bytes + 2 * page_size, page_size);
        vbo[i] = reinterpret_cast<GpuVertex*>(static_cast<u8*>(vbo_alloc[i]) + page_size);
    }

    C3D_TexInit(&white_tex, 8, 8, GPU_RGBA8);
    std::vector<u32> white(64, 0xFFFFFFFF);
    C3D_TexUpload(&white_tex, white.data());
    C3D_TexSetFilter(&white_tex, GPU_NEAREST, GPU_NEAREST);

}

// The last frame that drew each HUD part (index = region): the bottom screen keeps showing a part
// while the game hides its HUD (cutscenes, dialogue).
static std::shared_ptr<FrameData> overlay_source[4];

static bool force_redraw;
extern "C" void n64_top_redraw(void) { force_redraw = true; }
extern "C" C3D_RenderTarget* n64_top_target(void) { return top_target; }

static void draw_frame_data(const FrameData& frame, GpuVertex* buf, size_t capacity, float eye = 0.0f, bool copy = true);

// Stereoscopic 3D (main.c, from the 3D slider in the scenes that have it): 0 turns it off. The
// screen's 3D mode follows, and a change redraws the frame on screen.
extern "C" void n64_top_set_stereo(float separation) {
    bool was = stereo_separation > 0.0f, now = separation > 0.0f;
    if (now != was) gfxSet3D(now);
    if (separation != stereo_separation) force_redraw = true;
    stereo_separation = separation;
}

// The right eye's target while stereoscopic 3D is on (for the front end's own drawing), else NULL.
extern "C" C3D_RenderTarget* n64_top_target_right(void) { return stereo_separation > 0.0f ? top_target_right : NULL; }
static void restore_citro2d_state();
static void offscreen_service();

// Pokemon Snap's red in-focus dot in the frame on screen (see Renderer::Impl::publish).
extern "C" bool n64_top_red_dot(void) { return current_frame && current_frame->red_dot; }

extern "C" bool n64_top_render(bool frozen) {
    offscreen_service();
    bool fresh = false;
    if (!frozen) {
        std::shared_ptr<FrameData> latest = Renderer::take_published_frame();
        if (latest) {
            current_frame = std::move(latest);
            fresh = true;
            bool seen[4] = {};
            for (const DrawCmd& d : current_frame->overlay_draws) {
                if (d.overlay_region > 0 && d.overlay_region < 4 && d.vertex_count > 0) seen[d.overlay_region] = true;
            }
            for (int r = 1; r < 4; r++) {
                if (seen[r]) overlay_source[r] = current_frame;
            }
        }
    }
    frame_number++;
    if (frame_number % 60 == 0) {
        fprintf(stderr, "[perf] backend per 60 frames: %lu texture uploads, %lu palette re-uploads, %lu evictions, %lu draws, cache %u\n",
            stat_uploads, stat_reuploads, stat_evictions, stat_draws, (unsigned)texture_cache.size());
        stat_uploads = stat_reuploads = stat_evictions = stat_draws = 0;
    }

    // The top screen keeps showing what was last drawn on it (citro3d only presents render targets
    // drawn to in the current frame), so redraw only when the game has finished a new frame. The UI
    // loop runs faster than the game, and redrawing an unchanged N64 frame cost ~15 ms each time.
    static bool drawn_once;
    if (force_redraw) {
        force_redraw = false;
        drawn_once = false; // the credits covered the top screen: draw the last frame again
    }
    if (!fresh && drawn_once) return false;
    drawn_once = true;

    extern volatile uint32_t n64_isolate_hi; // development capture: magenta background (n64_renderer.cpp)
    u32 clear = (n64_isolate_hi || n64_debug_magenta) ? 0xFF00FFFF : 0x000000FF;
    C3D_RenderTargetClear(top_target, C3D_CLEAR_ALL, clear, 0x00FFFFFF);
    if (stereo_separation > 0.0f) C3D_RenderTargetClear(top_target_right, C3D_CLEAR_ALL, clear, 0x00FFFFFF);
    C3D_FrameDrawOn(top_target);
    if (!current_frame || current_frame->vertices.empty()) return true;

    // Left eye: far things shift left (the target's clip y is the screen's x, negated); the right
    // eye the other way, from the same vertices.
    draw_frame_data(*current_frame, vbo[vbo_index], vbo_capacity, stereo_separation);
    if (stereo_separation > 0.0f) {
        C3D_FrameDrawOn(top_target_right);
        draw_frame_data(*current_frame, vbo[vbo_index], vbo_capacity, -stereo_separation, false);
    }
    vbo_index ^= 1;
    restore_citro2d_state();
    return true;
}

// Draws a frame's draws into the current render target. `buf` is linear memory for its vertices
// (`capacity` of them) that the GPU reads during this citro3d frame.
static void draw_frame_data(const FrameData& frame, GpuVertex* buf, size_t capacity, float eye, bool copy) {
    const FrameData* current_frame = &frame;
    C3D_BindProgram(&program);
    C3D_SetAttrInfo(&attr_info);

    size_t count = std::min(current_frame->vertices.size(), capacity);
    if (copy) {
        memcpy(buf, current_frame->vertices.data(), count * sizeof(GpuVertex));
        GSPGPU_FlushDataCache(buf, count * sizeof(GpuVertex));
    }
    // Stereo shift per draw: 3D draws only (perspective, w != 1; also the particles at their depth);
    // 2D (HUD, menus, text, w = 1) and depth clears stay on the screen's plane.
    // Photos (2D images of a 3D scene) get one depth of their own, behind the screen. 2D drawn before
    // a frame's first 3D draw (a course's sky) is its background: as far as can be.
    int stereo_state = -1;
    auto set_stereo = [&](int mode) { // 0 flat, 1 per-vertex depth, 2 photo, 3 background
        int want = eye != 0.0f ? mode : 0;
        if (want == stereo_state || stereo_uniform < 0) return;
        stereo_state = want;
        if (want == 1) C3D_FVUnifSet(GPU_VERTEX_SHADER, stereo_uniform, eye, 0.0f, 1.0f, -eye); // n64.v.pica
        else if (want == 2) C3D_FVUnifSet(GPU_VERTEX_SHADER, stereo_uniform, 0.0f, eye * stereo_photo_depth, 1.0f, 0.0f);
        else if (want == 3) C3D_FVUnifSet(GPU_VERTEX_SHADER, stereo_uniform, 0.0f, eye, 1.0f + std::fabs(eye), 0.0f);
        else C3D_FVUnifSet(GPU_VERTEX_SHADER, stereo_uniform, 0.0f, 0.0f, 1.0f, 0.0f);
    };
    set_stereo(0);
    auto is_3d = [&](const DrawCmd& d) { return !d.depth_clear && std::fabs(buf[d.first_vertex].pos[3] - 1.0f) > 0.001f; };
    size_t first_3d = current_frame->draws.size();
    if (eye != 0.0f) {
        for (size_t i = 0; i < current_frame->draws.size(); i++) {
            const DrawCmd& d = current_frame->draws[i];
            if (d.vertex_count && d.first_vertex + d.vertex_count <= count && is_3d(d)) {
                first_3d = i;
                break;
            }
        }
    }

    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, buf, sizeof(GpuVertex), 4, 0x3210);
    C3D_CullFace(GPU_CULL_NONE);
    reset_state_cache();
    // W-buffer: depth = -z / 32768, linear in distance (z is in [-w, 0], n64_renderer.cpp). The
    // default z/w crowds distant depths near 1.0, where the GPU's 24-bit floats (16-bit mantissa)
    // left ~20 units of precision at 5000: Pokemon Snap's sea showed through the Beach's sand.
    C3D_DepthMap(false, -1.0f / 32768.0f, 0.0f);

    for (const DrawCmd& d : current_frame->draws) {
        if (d.vertex_count == 0 || d.first_vertex + d.vertex_count > count) continue;
        if (n64_debug_magenta && d.fill_mode) continue; // development: show what nothing covers
        setup_draw_state(d);
        if (n64_debug_drawid) {
            // Development: flat color = draw index (red = low byte, green = high byte), opaque.
            uint32_t idx = (uint32_t)(&d - current_frame->draws.data()) + 1;
            for (int s = 0; s < 6; s++) {
                C3D_TexEnv* env = C3D_GetTexEnv(s);
                C3D_TexEnvInit(env);
                if (s == 0) {
                    C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
                    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
                    C3D_TexEnvColor(env, 0xFF000000u | ((idx >> 8) & 0xFF) << 8 | (idx & 0xFF));
                }
            }
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            C3D_AlphaTest(false, GPU_ALWAYS, 0);
            reset_state_cache();
        }
        stat_draws++;
        size_t index = (size_t)(&d - current_frame->draws.data());
        set_stereo(d.photo_plane ? 2 : is_3d(d) ? 1 : index < first_3d && first_3d < current_frame->draws.size() ? 3 : 0);
        C3D_DrawArrays(GPU_TRIANGLES, d.first_vertex, d.vertex_count);
    }
}

// Leaves state the way citro2d expects for the bottom screen (C2D_Prepare doesn't reset these).
static void restore_citro2d_state() {
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_CullFace(GPU_CULL_BACK_CCW);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_DepthMap(true, -1.0f, 0.0f); // citro3d's default, for citro2d
    C3D_TexEnvBufUpdate(C3D_Both, 0);
    // Not nullptr: citro3d dereferences bound textures on the next draw (citro2d's bottom screen).
    C3D_TexBind(1, &white_tex);
}

// --- Render-to-RAM (n64_renderer.cpp, finish_offscreen_frames) -----------------------------------
// Images the game draws off screen (Pokemon Snap's photos, its thumbnail sheet and the strips cut
// from it) are drawn by the GPU and written back to RDRAM, where the game's own texture loads read
// them. One render target, made at start-up (making or freeing targets during play, and rendering
// into ones made from textures, froze the emulator): 256x512 portrait, so N64 pixel (x, y) of an
// image lies 1:1 at (x, y) of a 512x256 landscape area (DrawCmd::offscreen_map). Per UI frame one
// image: its current RDRAM contents are drawn first (the N64 draws onto what the image holds), then
// its draws; the GPU copies the target to linear memory behind them, and the next UI frame writes
// the image back (RGBA5551 -> RGBA8 -> RGBA5551 is lossless, so untouched pixels stay as they were).

static C3D_RenderTarget* off_target;
static GpuVertex* off_vbo;
static void* off_vbo_alloc;
static u32* off_readback;            // the target as linear RGBA8, 256 per row
static C3D_Tex off_seed;             // the image's current contents (512x256 RGBA8)
static std::shared_ptr<TexImage> off_seed_image;
static std::shared_ptr<FrameData> off_drawn; // drawn last UI frame, written back this one
static bool off_init_done;
constexpr size_t off_vbo_capacity = 16384;
constexpr size_t off_vbo_bytes = (off_vbo_capacity * sizeof(GpuVertex) + page_size - 1) & ~(page_size - 1);
constexpr uint32_t off_seed_rt = 0xFFFFFFFFu; // TexImage::rt_address of the seed texture

static void offscreen_init() {
    off_init_done = true;
    off_target = C3D_RenderTargetCreate(256, 512, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    off_vbo_alloc = linearMemAlign(off_vbo_bytes + 2 * page_size, page_size);
    off_vbo = off_vbo_alloc ? reinterpret_cast<GpuVertex*>(static_cast<u8*>(off_vbo_alloc) + page_size) : nullptr;
    off_readback = static_cast<u32*>(linearAlloc(256 * 512 * 4));
    bool seed_ok = C3D_TexInit(&off_seed, 512, 256, GPU_RGBA8);
    if (seed_ok) C3D_TexSetFilter(&off_seed, GPU_NEAREST, GPU_NEAREST);
    off_seed_image = std::make_shared<TexImage>();
    off_seed_image->key = 0xD000000000000000ull;
    off_seed_image->width = 512;
    off_seed_image->height = 256;
    off_seed_image->rt_address = off_seed_rt;
    if (!off_vbo || !off_readback || !seed_ok) off_target = nullptr;
    fprintf(stderr, "[gfx] render-to-RAM %s\n", off_target ? "ready" : "unavailable");
}

// get_texture() for TexImage::rt_address (only the seed texture uses it now).
static C3D_Tex* offscreen_texture(const TexBinding& b) {
    return b.image->rt_address == off_seed_rt ? &off_seed : &white_tex;
}

// The area the draws cover, in N64 pixels: each draw's bounding box within its scissor (the RDP
// writes nothing else; Pokemon Snap's strips are 64x24 images drawn with a full-screen scissor, and
// writing back more than they cover flattened the strips next to them).
struct OffRect { int x0, y0, x1, y1; };
static OffRect offscreen_rect(const FrameData& f) {
    OffRect r{ 512, 256, 0, 0 };
    for (const DrawCmd& d : f.draws) {
        if (!d.offscreen_map || d.depth_clear || d.vertex_count == 0) continue;
        float bx0 = 512, by0 = 256, bx1 = 0, by1 = 0;
        bool behind = false;
        for (uint32_t i = d.first_vertex; i < d.first_vertex + d.vertex_count && i < f.vertices.size(); i++) {
            const GpuVertex& g = f.vertices[i];
            if (g.pos[3] <= 0.0f) { behind = true; break; }
            float x = (1.0f - g.pos[1] / g.pos[3]) * 256.0f, y = (1.0f - g.pos[0] / g.pos[3]) * 128.0f;
            bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y);
        }
        if (behind) { bx0 = 0; by0 = 0; bx1 = 512; by1 = 256; }
        int x0 = std::max<int>((int)std::floor(bx0), d.scissor[0]), y0 = std::max<int>((int)std::floor(by0), d.scissor[1]);
        int x1 = std::min<int>((int)std::ceil(bx1), d.scissor[2]), y1 = std::min<int>((int)std::ceil(by1), d.scissor[3]);
        if (x0 >= x1 || y0 >= y1) continue;
        r.x0 = std::min(r.x0, x0); r.y0 = std::min(r.y0, y0); r.x1 = std::max(r.x1, x1); r.y1 = std::max(r.y1, y1);
    }
    r.x1 = std::min<int>(r.x1, (int)f.fb_width);
    return r;
}

// Linear read-back index of N64 pixel (x, y): the portrait target has column 255 - y, and the
// copy's rows run from the top, row x. (Checked with offscreen_raw.bin.)
static inline u32 off_read(int x, int y) { return off_readback[x * 256 + (255 - y)]; }


static void offscreen_write_back(FrameData& f) {
    GSPGPU_InvalidateDataCache(off_readback, 256 * 512 * 4);
    OffRect area = offscreen_rect(f);
    static bool dumped;
    if (!dumped && f.fb_width == 320 && f.draws.size() > 20) { // development: a photo, as landscape RGBA8 (512x256)
        dumped = true;
        FILE* file = fopen("sdmc:/3ds/psnap/offscreen_raw.bin", "wb");
        if (file) {
            for (int y = 0; y < 256; y++) {
                for (int x = 0; x < 512; x++) {
                    u32 c = off_read(x, y);
                    fwrite(&c, 4, 1, file);
                }
            }
            fclose(file);
        }
    }
    for (int y = area.y0; y < area.y1; y++) {
        for (int x = area.x0; x < area.x1; x++) {
            u32 c = off_read(x, y);
            uint8_t r = c >> 24, g = c >> 16, b = c >> 8;
            uint16_t px = (uint16_t)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | ((c & 0xFF) >= 0x80 ? 1 : 0));
            uint32_t addr = (f.fb_address + (y * (uint32_t)f.fb_width + x) * 2) & 0x00FFFFFF;
            *reinterpret_cast<uint16_t*>(f.rdram + (addr ^ 2)) = px;
        }
    }
    f.written = true; // the graphics thread may be waiting for it (sync_offscreen_reads)
}

// The image's current RDRAM contents into the seed texture (texel layout as tiled_offset in
// n64_renderer.cpp: 8x8 Morton tiles, rows stored bottom first).
static void offscreen_seed(const FrameData& f, const OffRect& area) {
    u32* texels = static_cast<u32*>(off_seed.data);
    for (int y = 0; y < 256; y++) {
        int ty = 255 - y;
        for (int x = 0; x < 512; x++) {
            u32 c = 0;
            if (x >= area.x0 && x < area.x1 && y >= area.y0 && y < area.y1) {
                uint32_t addr = (f.fb_address + (y * f.fb_width + x) * 2) & 0x00FFFFFF;
                uint16_t p = *reinterpret_cast<const uint16_t*>(f.rdram + (addr ^ 2));
                u32 r = (p >> 11) & 31, g = (p >> 6) & 31, b = (p >> 1) & 31;
                r = r << 3 | r >> 2; g = g << 3 | g >> 2; b = b << 3 | b >> 2;
                c = r << 24 | g << 16 | b << 8 | ((p & 1) ? 0xFF : 0x00);
            }
            uint32_t tile = (uint32_t)(ty >> 3) * (512 >> 3) + (uint32_t)(x >> 3);
            uint32_t morton = (x & 1) | ((ty & 1) << 1) | ((x & 2) << 1) | ((ty & 2) << 2) | ((x & 4) << 2) | ((ty & 4) << 3);
            texels[tile * 64 + morton] = c;
        }
    }
    GSPGPU_FlushDataCache(off_seed.data, off_seed.size);
}

// (Kept for the main loop's call.)
extern "C" void n64_offscreen_before_frame(void) {}

// Once per UI frame, inside the citro3d frame, before the top screen.
static void offscreen_service() {
    if (!off_init_done) offscreen_init();
    if (off_drawn) {
        offscreen_write_back(*off_drawn);
        off_drawn.reset();
    }
    if (!off_target) {
        while (auto f = Renderer::take_offscreen_frame()) f->written = true;
        return;
    }
    std::shared_ptr<FrameData> f = Renderer::take_offscreen_frame();
    if (!f) return;
    if (f->vertices.empty() || f->vertices.size() + 6 > off_vbo_capacity) {
        f->written = true;
        return;
    }
    OffRect area = offscreen_rect(*f);
    if (area.x0 >= area.x1 || area.y0 >= area.y1) {
        f->written = true;
        return;
    }
    offscreen_seed(*f, area);
    // The seed as the first draw: a COPY-mode quad over the image area, 1:1.
    FrameData frame_with_seed;
    frame_with_seed.vertices.reserve(f->vertices.size() + 6);
    auto corner = [&](float x, float y) {
        GpuVertex g{};
        float cx = x / 256.0f - 1.0f, cy = 1.0f - y / 128.0f; // DrawCmd::offscreen_map
        g.pos[0] = cy; g.pos[1] = -cx; g.pos[2] = 0.0f; g.pos[3] = 1.0f;
        g.uv0[0] = x / 512.0f; g.uv0[1] = y / 256.0f;
        g.color[0] = g.color[1] = g.color[2] = g.color[3] = 255;
        return g;
    };
    float ax0 = (float)area.x0, ay0 = (float)area.y0, ax1 = (float)area.x1, ay1 = (float)area.y1;
    for (const GpuVertex& g : { corner(ax0, ay0), corner(ax0, ay1), corner(ax1, ay1),
                                corner(ax0, ay0), corner(ax1, ay1), corner(ax1, ay0) }) {
        frame_with_seed.vertices.push_back(g);
    }
    DrawCmd seed{};
    seed.first_vertex = 0;
    seed.vertex_count = 6;
    seed.copy_mode = true;
    seed.blend = BlendMode::Opaque;
    seed.offscreen_map = true;
    seed.scissor[0] = 0; seed.scissor[1] = 0; seed.scissor[2] = 512; seed.scissor[3] = 256;
    seed.tex[0].image = off_seed_image;
    seed.tex[0].wrap_s = seed.tex[0].wrap_t = TexWrap::Clamp;
    seed.tex[0].linear = false;
    frame_with_seed.draws.push_back(seed);
    for (DrawCmd d : f->draws) {
        d.first_vertex += 6;
        frame_with_seed.draws.push_back(std::move(d));
    }
    frame_with_seed.vertices.insert(frame_with_seed.vertices.end(), f->vertices.begin(), f->vertices.end());

    C3D_RenderTargetClear(off_target, C3D_CLEAR_ALL, 0x000000FF, 0x00FFFFFF);
    C3D_FrameDrawOn(off_target);
    C3D_SetViewport(0, 0, 256, 512);
    draw_frame_data(frame_with_seed, off_vbo, off_vbo_capacity);
    restore_citro2d_state();
    C3D_FrameSplit(0); // the draws first, then the copy to linear memory behind them
    GX_DisplayTransfer((u32*)off_target->frameBuf.colorBuf, GX_BUFFER_DIM(256, 512), off_readback, GX_BUFFER_DIM(256, 512),
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    off_drawn = std::move(f);
}

// Draws a HUD part diverted from the top screen (FrameData::overlay_draws, region 1 life,
// 2 weapon energy, 3 minimap) into the current render target - the bottom screen, inside a citro2d scene.
// The gauge's top-screen pixel area starting at (src_x, src_y) is placed at (dst_x, dst_y) on the
// 320x240 bottom screen, scaled by `scale`. Returns false if the frame has no such gauge.
extern "C" bool n64_bottom_overlay(int region, float src_x, float src_y, float dst_x, float dst_y, float scale) {
    if (region < 1 || region > 3 || !overlay_source[region]) return false;
    const FrameData& f = *overlay_source[region];
    // Vertices are re-uploaded each UI frame into a small buffer; C3D_FRAME_SYNCDRAW makes the
    // previous frame's use of it finish first, and each call this frame takes the next part.
    constexpr size_t overlay_capacity = 4096;
    static GpuVertex* buf = static_cast<GpuVertex*>(linearAlloc(overlay_capacity * sizeof(GpuVertex)));
    static u32 buf_frame = ~0u;
    static size_t used = 0;
    if (buf_frame != frame_number) {
        buf_frame = frame_number;
        used = 0;
    }
    bool any = false;
    for (const DrawCmd& d : f.overlay_draws) any |= d.overlay_region == region && d.vertex_count > 0;
    if (!any || !buf) return false;
    {   // Development: log the gauge's extent in top-screen pixels now and then.
        static u32 logged_frame[4];
        if (region > 0 && region < 4 && frame_number - logged_frame[region] > 600) {
            logged_frame[region] = frame_number;
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (const DrawCmd& d : f.overlay_draws) {
                if (d.overlay_region != region) continue;
                for (uint32_t i = 0; i < d.vertex_count; i++) {
                    const GpuVertex& g = f.overlay_vertices[d.first_vertex + i];
                    float tx = (-g.pos[1] / g.pos[3] + 1.0f) * 200.0f, ty = (1.0f - g.pos[0] / g.pos[3]) * 120.0f;
                    x0 = std::min(x0, tx); x1 = std::max(x1, tx); y0 = std::min(y0, ty); y1 = std::max(y1, ty);
                }
            }
            fprintf(stderr, "[hud] part %d spans x %.0f-%.0f y %.0f-%.0f (%u draws)\n", region, x0, x1, y0, y1, (unsigned)f.overlay_draws.size());
        }
    }

    C2D_Flush(); // keep citro2d's queued drawing in order before switching shaders
    C3D_BindProgram(&program);
    C3D_SetAttrInfo(&attr_info);
    C3D_CullFace(GPU_CULL_NONE);
    reset_state_cache();
    size_t start = used;
    for (const DrawCmd& d : f.overlay_draws) {
        if (d.overlay_region != region || d.vertex_count == 0) continue;
        if (used + d.vertex_count > overlay_capacity) break;
        size_t first = used;
        for (uint32_t i = 0; i < d.vertex_count; i++) {
            GpuVertex g = f.overlay_vertices[d.first_vertex + i];
            // Top-screen clip space (portrait: pos0 = landscape y, pos1 = -landscape x) -> pixels,
            // placed on the bottom screen, back to its clip space (same portrait convention).
            float tx = (-g.pos[1] / g.pos[3] + 1.0f) * 200.0f, ty = (1.0f - g.pos[0] / g.pos[3]) * 120.0f;
            float bx = dst_x + (tx - src_x) * scale, by = dst_y + (ty - src_y) * scale;
            g.pos[0] = 1.0f - by / 120.0f;
            g.pos[1] = -(bx / 160.0f - 1.0f);
            g.pos[2] = 0.0f;
            g.pos[3] = 1.0f;
            buf[used++] = g;
        }
        DrawCmd dd = d;
        dd.scissor[0] = 0; dd.scissor[1] = 0; dd.scissor[2] = 400; dd.scissor[3] = 240; // no clipping
        dd.depth_test = dd.depth_write = false;
        C3D_BufInfo* bi = C3D_GetBufInfo();
        BufInfo_Init(bi);
        BufInfo_Add(bi, buf + first, sizeof(GpuVertex), 4, 0x3210);
        setup_draw_state(dd);
        C3D_DrawArrays(GPU_TRIANGLES, 0, d.vertex_count);
    }
    GSPGPU_FlushDataCache(buf + start, (used - start) * sizeof(GpuVertex));

    // Leave state the way citro2d expects (as at the end of n64_top_render).
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_CullFace(GPU_CULL_BACK_CCW);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_TexEnvBufUpdate(C3D_Both, 0);
    C3D_TexBind(1, &white_tex);
    C2D_Prepare();
    return true;
}

extern "C" void n64_top_exit(void) {
    for (auto& [key, ct] : texture_cache) {
        C3D_TexDelete(&ct->tex);
        delete ct;
    }
    texture_cache.clear();
    C3D_TexDelete(&white_tex);
    for (auto& buf : vbo_alloc) linearFree(buf);
    shaderProgramFree(&program);
    DVLB_Free(shader_dvlb);
    C3D_RenderTargetDelete(top_target);
    C3D_RenderTargetDelete(top_target_right);
}

extern "C" void n64_top_stats(unsigned* draws, unsigned* vertices) {
    *draws = current_frame ? (unsigned)current_frame->draws.size() : 0;
    *vertices = current_frame ? (unsigned)current_frame->vertices.size() : 0;
}

extern "C" void n64_top_dump_current(void) {
    if (!current_frame) return;
    const FrameData& f = *current_frame;
    fprintf(stderr, "[shown] fb=0x%06X draws=%u verts=%u\n", (unsigned)f.fb_address, (unsigned)f.draws.size(), (unsigned)f.vertices.size());
    for (const DrawCmd& d : f.overlay_draws) { // HUD parts moved to the bottom screen
        fprintf(stderr, "[overlay] region %d n=%lu tex=%dx%d src=%06X prim=%02X%02X%02X%02X\n", d.overlay_region, (unsigned long)d.vertex_count,
            d.tex[0].image ? d.tex[0].image->width : 0, d.tex[0].image ? d.tex[0].image->height : 0,
            d.tex[0].image ? (unsigned)d.tex[0].image->src : 0u, d.prim[0], d.prim[1], d.prim[2], d.prim[3]);
    }
    int n = 0;
    for (const DrawCmd& d : f.draws) {
        if (n++ >= 600) break;
        const GpuVertex& v = f.vertices[d.first_vertex];
        fprintf(stderr, "[shown] %3d n=%-4lu %s%s%s cc=%d,%d,%d,%d/%d,%d,%d,%d cc2=%d,%d,%d,%d/%d,%d,%d,%d tex=%dx%d f%d/s%d op%d%% L%d prim=%02X%02X%02X%02X env=%02X%02X%02X%02X bl=%d om=%08lX gm=%06lX at=%d/%d zt=%d zw=%d zc=%d sc=%d,%d,%d,%d v0=(%.2f %.2f %.3f %.2f) c=%02X%02X%02X%02X src=%06X\n",
            n, d.vertex_count, d.copy_mode ? "C" : "", d.fill_mode ? "F" : "", d.two_cycle ? "2" : "1",
            d.cycles[0].rgb[0], d.cycles[0].rgb[1], d.cycles[0].rgb[2], d.cycles[0].rgb[3],
            d.cycles[0].alpha[0], d.cycles[0].alpha[1], d.cycles[0].alpha[2], d.cycles[0].alpha[3],
            d.cycles[1].rgb[0], d.cycles[1].rgb[1], d.cycles[1].rgb[2], d.cycles[1].rgb[3],
            d.cycles[1].alpha[0], d.cycles[1].alpha[1], d.cycles[1].alpha[2], d.cycles[1].alpha[3],
            d.tex[0].image ? d.tex[0].image->width : 0, d.tex[0].image ? d.tex[0].image->height : 0,
            d.tex[0].image ? d.tex[0].image->fmt : -1, d.tex[0].image ? d.tex[0].image->siz : -1,
            d.tex[0].image ? d.tex[0].image->opaque_pct : -1,
            d.tex[0].image ? d.tex[0].image->mean_luma : -1,
            d.prim[0], d.prim[1], d.prim[2], d.prim[3], d.env[0], d.env[1], d.env[2], d.env[3],
            (int)d.blend, d.other_mode_l, (unsigned long)d.geometry_mode, d.alpha_test, d.alpha_ref, d.depth_test, d.depth_write, d.depth_clear,
            d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3],
            v.pos[0], v.pos[1], v.pos[2], v.pos[3], v.color[0], v.color[1], v.color[2], v.color[3],
            d.tex[0].image ? (unsigned)d.tex[0].image->src : 0u);
        if (d.tex[0].image && d.vertex_count >= 3) {
            const GpuVertex* t = &f.vertices[d.first_vertex];
            fprintf(stderr, "[shown]     wrap=%d,%d lin=%d uv=(%.3f %.3f)(%.3f %.3f)(%.3f %.3f) w=%.2f,%.2f,%.2f\n",
                (int)d.tex[0].wrap_s, (int)d.tex[0].wrap_t, (int)d.tex[0].linear,
                t[0].uv0[0], t[0].uv0[1], t[1].uv0[0], t[1].uv0[1], t[2].uv0[0], t[2].uv0[1], t[0].pos[3], t[1].pos[3], t[2].pos[3]);
        }
    }
}

// Development: write every texture used by the frame currently shown to `dir` as raw RGBA8
// (top row first, unpadded size), named by draw count, size, RDRAM source and N64 format.
extern "C" void n64_top_dump_textures(const char* dir) {
    if (!current_frame) return;
    std::unordered_map<const TexImage*, unsigned> uses;
    std::vector<const TexImage*> order;
    for (const DrawCmd& d : current_frame->draws) {
        const TexImage* img = d.tex[0].image.get();
        if (!img) continue;
        if (uses[img]++ == 0) order.push_back(img);
    }
    for (const TexImage* img : order) {
        char path[192];
        snprintf(path, sizeof(path), "%s/tex_%06X_%ux%u_f%us%u_x%u.rgba", dir, (unsigned)img->src,
            (unsigned)img->src_w, (unsigned)img->src_h, (unsigned)img->fmt, (unsigned)img->siz, uses[img]);
        FILE* f = fopen(path, "wb");
        if (!f) continue;
        for (uint32_t y = 0; y < img->src_h; y++) {
            for (uint32_t x = 0; x < img->src_w; x++) {
                // Inverse of n64_renderer.cpp's tiled_offset (rows stored bottom first).
                uint32_t ty = img->height - 1 - y;
                uint32_t tile = (ty >> 3) * (img->width >> 3) + (x >> 3);
                uint32_t morton = (x & 1) | ((ty & 1) << 1) | ((x & 2) << 1) | ((ty & 2) << 2) | ((x & 4) << 2) | ((ty & 4) << 3);
                uint32_t t = img->texels[tile * 64 + morton];
                uint8_t px[4] = { (uint8_t)(t >> 24), (uint8_t)(t >> 16), (uint8_t)(t >> 8), (uint8_t)t };
                fwrite(px, 1, 4, f);
            }
        }
        fclose(f);
    }
    fprintf(stderr, "[tex] dumped %u textures to %s\n", (unsigned)order.size(), dir);
}
