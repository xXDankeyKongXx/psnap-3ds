#ifndef N64_RENDERER_HPP
#define N64_RENDERER_HPP

// F3DEX2/RDP display-list interpreter. Runs on ultramodern's graphics thread: walks each display
// list, does transform & lighting on the CPU, decodes textures, and builds an n64r::FrameData per
// N64 framebuffer. When the VI starts showing a framebuffer, its FrameData is published for the
// main thread's Citro3D backend to draw.

#include <cstdint>
#include <memory>

#include "n64_frame.hpp"

namespace n64r {

class Renderer {
public:
    explicit Renderer(uint8_t* rdram);
    ~Renderer();

    // Interpret one graphics task's display list (data_ptr of the OSTask).
    void run_display_list(uint32_t dl_address);

    // Called on each VI update; publishes the frame drawn into the framebuffer at vi_origin.
    void on_vi(uint32_t vi_origin);

    // Main thread: take the most recently published frame (null if none since the last call).
    static std::shared_ptr<FrameData> take_published_frame();

    // Main thread: take the oldest frame drawn into an off-screen color image (render-to-RAM: the
    // backend renders it and writes the pixels to RDRAM at fb_address). Null if none is waiting.
    static std::shared_ptr<FrameData> take_offscreen_frame();

    // Development: logs what the renderer holds in memory (runtime.log).
    static void mem_report();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace n64r

#endif
