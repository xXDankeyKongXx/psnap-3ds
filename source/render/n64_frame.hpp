#ifndef N64_FRAME_HPP
#define N64_FRAME_HPP

// Data handed from the display-list interpreter (graphics thread) to the Citro3D backend (main
// thread). Everything here is described in N64 terms; the backend translates it to PICA200 state.
// No libctru/citro3d or ultramodern headers here so both sides can include it.

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace n64r {

// Vertex as uploaded to the GPU: clip-space position already mapped for the 3DS top screen.
struct GpuVertex {
    float pos[4];
    float uv0[2];
    float uv1[2];
    uint8_t color[4]; // shade RGBA
};

// Normalized color-combiner inputs (both RGB and alpha slots use this set).
enum CcInput : uint8_t {
    CC_ZERO, CC_ONE, CC_COMBINED, CC_TEX0, CC_TEX1, CC_PRIM, CC_SHADE, CC_ENV,
    CC_COMBINED_A, CC_TEX0_A, CC_TEX1_A, CC_PRIM_A, CC_SHADE_A, CC_ENV_A,
    CC_LOD_FRAC, CC_PRIM_LOD_FRAC, CC_NOISE, CC_K4, CC_K5, CC_CENTER, CC_SCALE,
};

// One combiner cycle: color = (a - b) * c + d, separately for RGB and alpha.
struct CombinerCycle {
    CcInput rgb[4];
    CcInput alpha[4];
};

// Decoded texture in PICA200 layout: RGBA8, 8x8 Morton-tiled, power-of-two size.
struct TexImage {
    uint64_t key;
    uint16_t width, height;
    std::vector<uint32_t> texels;
    // Diagnostics
    uint8_t fmt, siz;      // N64 image format/size it was decoded from
    uint32_t src, stride;  // RDRAM address and row stride of the source texels
    uint16_t src_w, src_h; // size before padding to a power of two
    uint8_t opaque_pct;    // share of texels with alpha >= 128
    uint8_t mean_luma;     // average brightness
    uint32_t last_used = 0; // display list that last bound it (renderer cache eviction)
    // Palette textures: the same for every palette of the same indices (0 for other formats). The
    // backend keeps one GPU texture per slot and re-uploads it when only the palette changed.
    uint64_t gpu_slot = 0;
    // Render-to-texture: a texture read from an off-screen image the game drew (no texels here).
    // The backend binds that image's render target; texture coordinates already point into it.
    uint32_t rt_address = 0;
};

enum class TexWrap : uint8_t { Repeat, Mirror, Clamp };

struct TexBinding {
    std::shared_ptr<TexImage> image; // null when this texture slot is unused
    TexWrap wrap_s = TexWrap::Repeat;
    TexWrap wrap_t = TexWrap::Repeat;
    bool linear = true;
};

// Result of decoding the RDP blender (see n64_renderer.cpp, decode_blender).
enum class BlendMode : uint8_t {
    Opaque,         // write the combiner output
    Alpha,          // out * a + mem * (1 - a), a = combiner alpha
    ConstantAlpha,  // out * a + mem * (1 - a), a = blend_alpha (fog/blend color alpha)
    KeepMemory,     // the blender selects the framebuffer: write no color
};

struct DrawCmd {
    uint32_t first_vertex;
    uint32_t vertex_count;

    bool two_cycle;
    bool copy_mode;    // texrect in COPY mode: output the texel directly
    bool fill_mode;    // fillrect in FILL mode: output the fill color (in prim)
    CombinerCycle cycles[2];
    uint8_t prim[4];
    uint8_t env[4];
    uint8_t prim_lod_frac;

    TexBinding tex[2];

    BlendMode blend;
    uint8_t blend_alpha;   // for BlendMode::ConstantAlpha
    uint32_t other_mode_l; // raw RDP render mode (diagnostics)
    bool alpha_test;       // discard below alpha_ref
    uint8_t alpha_ref;
    bool depth_test;
    bool depth_write;
    bool decal;
    bool depth_clear;      // N64 fillrect into the Z buffer: reset depth, draw no color
    uint32_t geometry_mode; // RSP geometry mode when the draw started (diagnostics)

    // Scissor in 3DS top-screen pixels (x across 400, y down 240).
    uint16_t scissor[4];

    uint8_t overlay_region = 0; // HUD part moved to the bottom screen: 1 life, 2 weapon energy, 3 minimap
    bool photo_plane = false;   // a photo (Album, Camera Check): drawn at its own stereoscopic depth
    bool seed = false;          // (unused)
    // Draw into an off-screen image: positions and scissor map N64 pixel (x, y) 1:1 into a 512x256
    // landscape area (the backend's 256x512 portrait render target), not onto the top screen.
    bool offscreen_map = false;
};

struct FrameData {
    std::vector<GpuVertex> vertices;
    std::vector<DrawCmd> draws;
    // HUD gauges diverted to the bottom screen (Renderer::hud_region): drawn there instead of on
    // the top screen, with their own vertices. Positions are still top-screen clip space.
    std::vector<GpuVertex> overlay_vertices;
    std::vector<DrawCmd> overlay_draws;
    uint32_t fb_address = 0;
    uint32_t fb_width = 320;   // N64 color image width in pixels
    uint8_t* rdram = nullptr;  // off-screen frames: where the backend writes the rendered pixels
    std::atomic<bool> written{ false }; // off-screen frames: set by the backend once in RDRAM
    bool red_dot = false; // Pokemon Snap's in-focus dot, which the game paints into the framebuffer (see publish)
    // Renderer cache for its image size: rows reached by draws[0, rows_scanned) (image_bytes).
    size_t rows_scanned = 0;
    uint32_t rows = 1;
};

} // namespace n64r

#endif
