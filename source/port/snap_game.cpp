// Pokemon Snap game glue for the 3DS runtime: game entry, overlay bookkeeping, game-specific hooks
// and the platform callbacks (renderer, RSP, audio, input).

#include <3ds.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <exception>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "recomp.h"
#include "funcs.h"
#include "librecomp/overlays.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultramodern.hpp"
#include "recomp_3ds.hpp"
#include "n64_renderer.hpp"
#include "audio_3ds.hpp"
#include "input_3ds.hpp"

#include "snap_game.h"
#include "photo_cache.h"
#include "snap_hooks.h"
#include "audio_hle.hpp"
#include "log_ring.h"
#include "trace.h"

#define ARRLEN(x) (sizeof(x) / sizeof((x)[0]))

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();
RspExitReason aspMain(uint8_t* rdram, uint32_t ucode_addr);

#include "recomp_overlays.inl"

extern uint32_t recomp3ds_boot_code_size; // runtime/librecomp/recomp_3ds.cpp

static std::atomic<unsigned> display_lists_seen{0};
static std::atomic<unsigned> display_lists_done{0};
static std::atomic<unsigned> frames_presented{0};
static std::atomic<unsigned> audio_tasks_run{0};

// Renderer features written for Mega Man 64 (HUD moved to the bottom screen, narration skip):
// off for Snap until it gets its own.
extern "C" {
volatile bool snap_boot_skip = false;
volatile uint32_t snap_hud_gauge_passes = 0, snap_hud_map_passes = 0;
}

// --- Overlays ------------------------------------------------------------------------------------
// Snap stacks differently sized overlays at overlapping addresses (a 0x6A00 menu inside the 0x14160
// .world region, .oaks_lab 0x20 above it, ...). librecomp refuses to unload part of a section, so
// the sections registered by each load are remembered, and a new load first evicts each resident
// section it overlaps, by that section's own extent. (Same approach as Snap64Recomp.)

struct Resident { uint32_t rom, ram, size; };
static std::vector<Resident> resident;
static std::mutex overlay_mutex;

static uint32_t read_word(uint8_t* rdram, uint32_t addr) {
    return *reinterpret_cast<uint32_t*>(rdram + (addr - 0x80000000u));
}

// dmaLoadOverlay(Overlay* a0) entry: Overlay = { romStart, romEnd, vramStart, textVramStart, ... }.
// The game's overlay descriptors are a table at 0x800ABBD0 (0x24 bytes each, app_render/46270.c):
// 3 Oak's Lab, 16-29 each course's assets and code (Beach, Tunnel, Cave, River, Volcano, Valley,
// Rainbow Cloud). Every scene loads its own overlay as it starts; shared ones (window, world,
// app_level, more_funcs) leave the scene as it was.
// Boot, the intro and the title / main menu / new game screens (10, 11, 13) count as the title.
static std::atomic<int> scene_kind{ SNAP_SCENE_TITLE }, scene_course{ -1 };
static std::atomic<int> scene_overlay{ -1 }; // index of the last scene overlay (not the shared ones)

static void track_scene(uint32_t descriptor) {
    if (descriptor < 0x800ABBD0u || (descriptor - 0x800ABBD0u) % 0x24 != 0) return;
    uint32_t index = (descriptor - 0x800ABBD0u) / 0x24;
    fprintf(stderr, "[scene] overlay %u\n", (unsigned)index);
    // A course starts with its code overlay (the odd one of its pair), which loads its data half (the
    // even one). Camera Check, Prof. Oak's Check and the Album load that data half too, to render the
    // photos: it never starts or ends a scene.
    if (index >= 16 && index <= 29 && index % 2 == 0) return;
    if (!(index == 1 || index == 9 || index == 14 || index == 15)) {
        scene_overlay = (int)index;
        photo_bitmaps_clear();
    }
    static const int course_scene[7] = { 0, 1, 4, 3, 2, 5, 6 }; // overlay order -> scene id
    if (index == 3) {
        scene_kind = SNAP_SCENE_LAB;
        scene_course = -1;
    } else if (index >= 16 && index <= 29) {
        scene_kind = SNAP_SCENE_COURSE;
        scene_course = course_scene[(index - 16) / 2];
    } else if (index == 10 || index == 11 || index == 13) {
        scene_kind = SNAP_SCENE_TITLE;
        scene_course = -1;
    } else if (index == 1 || index == 9 || index == 14 || index == 15) {
        return; // shared
    } else {
        scene_kind = SNAP_SCENE_OTHER;
        scene_course = -1;
    }
}

extern "C" SnapScene snap_scene(void) { return (SnapScene)scene_kind.load(); }

// Overlays: 2 Camera Check, 4 Pokemon Album, 10 main menu (the attract intro; its title and menus are
// 2D, so they stay flat), 13 boot; courses 16-29.
extern "C" int snap_scene_overlay(void) { return scene_overlay.load(); }

extern "C" bool snap_stereo_scene(void) {
    int o = scene_overlay.load();
    return scene_kind.load() == SNAP_SCENE_COURSE || o == 2 || o == 4 || o == 10 || o == 13;
}

// Course HUD (app_level/icons.c): Icons_MainObject at 0x80388230 draws the button icons; the apple,
// Pester Ball and flute sprite objects are Icons_ButtonIcons[0..2] (0x80388224), the dash icons
// Icons_IconObjects[4..5].spriteObj (0x8038812C + i * 0x18 + 8). Sprite attributes are at +0x24 of a
// sprite object (SObj.sprite at 0x10, Sprite.attr at 0x14). The items are on the bottom screen in
// courses (bottom_ui.c), so they're hidden each time the icon object is drawn.
extern "C" void snap_on_item(int kind) {
    static unsigned item_input_calls;
    if (kind == 2) { // handleItemButtonsPress
        if (item_input_calls++ % 300 == 0) fprintf(stderr, "[item] item input process: %u calls\n", item_input_calls);
        return;
    }
    static const char* const names[2] = { "apple", "Pester Ball" };
    fprintf(stderr, "[item] %s thrown\n", names[kind & 1]);
}

extern "C" void snap_on_draw_sprite(uint8_t* rdram, recomp_context* ctx) {
    if (scene_kind.load() != SNAP_SCENE_COURSE) return;
    uint32_t gobj = (uint32_t)ctx->r4;
    if (gobj == 0 || gobj != read_word(rdram, 0x80388230)) return;
    uint32_t sobjs[5] = { read_word(rdram, 0x80388224), read_word(rdram, 0x80388228), read_word(rdram, 0x8038822C),
                          read_word(rdram, 0x8038812C + 4 * 0x18 + 8), read_word(rdram, 0x8038812C + 5 * 0x18 + 8) };
    for (uint32_t s : sobjs) {
        if (s < 0x80000000u || s >= 0x80800000u) continue;
        uint32_t a = (s + 0x24) & 0x00FFFFFF; // u16 at an aligned word's upper half
        *reinterpret_cast<uint16_t*>(rdram + (a ^ 2)) |= 0x0004; // SP_HIDDEN
    }
}
extern "C" int snap_course(void) { return scene_course.load(); }

extern "C" void snap_on_overlay_load(uint8_t* rdram, recomp_context* ctx) {
    uint32_t ov = (uint32_t)ctx->r4;
    track_scene(ov);
    uint32_t rom_start = read_word(rdram, ov + 0x0), rom_end = read_word(rdram, ov + 0x4);
    uint32_t vram = read_word(rdram, ov + 0x8);
    if (rom_end <= rom_start) return;
    uint32_t size = rom_end - rom_start;
    std::lock_guard lock{ overlay_mutex };
    for (auto it = resident.begin(); it != resident.end();) {
        if (it->ram < vram + size && vram < it->ram + it->size) {
            unload_overlays((int32_t)it->ram, it->size);
            it = resident.erase(it);
        } else {
            ++it;
        }
    }
    load_overlays(rom_start, (int32_t)vram, size);
    unsigned added = 0;
    for (const SectionTableEntry& s : section_table) {
        if (s.size == 0 || s.rom_addr < rom_start || s.rom_addr >= rom_end) continue;
        resident.push_back({ s.rom_addr, s.rom_addr - rom_start + vram, s.size });
        added++;
    }
    fprintf(stderr, "[overlay] desc %08lX scene %d rom %06lX-%06lX -> %08lX: %u section(s), %u resident\n", (unsigned long)ov,
        scene_kind.load(), (unsigned long)rom_start,
        (unsigned long)rom_end, (unsigned long)vram, added, (unsigned)resident.size());
}

extern "C" void snap_debug_dump_overlays(void) {
    std::lock_guard lock{ overlay_mutex };
    fprintf(stderr, "[overlays]");
    for (const Resident& r : resident) fprintf(stderr, " %06lX@%08lX", (unsigned long)r.rom, (unsigned long)r.ram);
    fprintf(stderr, "\n");
}

// auThreadMain's two AI_LEN reads (recomp/pokemonsnap.us.3ds.toml): bytes the AI still has queued.
extern "C" uint32_t snap_ai_length(void) {
    return (uint32_t)ultramodern::get_remaining_audio_bytes();
}

// game_main sets a CPU watchpoint (a debugging aid on the N64): nothing to do here.
extern "C" void __osSetWatchLo_recomp(uint8_t*, recomp_context*) {}

// Controller Pak block transfers: no pak here, so PFS_ERR_NOPACK (1) in v0, as Snap64Recomp answers.
extern "C" void __osContRamRead_recomp(uint8_t*, recomp_context* ctx) { ctx->r2 = 1; }
extern "C" void __osContRamWrite_recomp(uint8_t*, recomp_context* ctx) { ctx->r2 = 1; }

// --- Renderer: F3DEX2 interpreter on the graphics thread (source/render/n64_renderer.cpp) -------

extern "C" void n64_renderer_profile_dump(unsigned dl_count); // n64_renderer.cpp

class N64RendererContext : public ultramodern::renderer::RendererContext {
    n64r::Renderer renderer;
public:
    explicit N64RendererContext(uint8_t* rdram) : renderer(rdram) {
        setup_result = ultramodern::renderer::SetupResult::Success;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;
    }
    bool valid() override { return true; }
    bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override { return false; }
    void enable_instant_present() override {}
    void send_dl(const OSTask* task) override {
        display_lists_seen++;
        auto start = std::chrono::steady_clock::now();
        renderer.run_display_list((uint32_t)task->t.data_ptr);
        long long us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        dl_time_us += us;
        display_lists_done++;
        if (us > 100000) {
            fprintf(stderr, "[perf] slow display list #%u: %lld ms\n", display_lists_seen.load(), us / 1000);
        }
        if (display_lists_seen % 60 == 0) {
            fprintf(stderr, "[perf] display list interpretation: avg %lld us over 60 DLs\n", dl_time_us / 60);
            n64_renderer_profile_dump(60);
            dl_time_us = 0;
        }
    }
    long long dl_time_us = 0;
    void send_dummy_workload(uint32_t) override {}
    void update_screen() override {
        frames_presented++;
        renderer.on_vi(ultramodern::renderer::get_vi_regs()->VI_ORIGIN_REG);
    }
    void shutdown() override {}
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1.0f; }
};

static std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle, bool) {
    return std::make_unique<N64RendererContext>(rdram);
}

// --- RSP: audio tasks run natively (audio_hle.cpp) ----------------------------------------------
// Development: with DATA_PATH/audio_compare.txt holding a number N, the first N tasks also run the
// recompiled microcode (recomp/rsp/aspMain.cpp) and differences are logged; with "lle" in it, every
// task runs the recompiled microcode only.

static std::atomic<uint64_t> audio_rsp_ticks{0};
static std::atomic<bool> audio_running; // sampled by the profiler thread
static uint32_t audio_task_data, audio_task_size; // set by get_rsp_microcode for the timed run
static unsigned audio_compare_left;
static bool audio_lle_only;
static bool audio_off; // development: "off" skips audio tasks (silence), to measure their cost

static RspExitReason timed_aspMain(uint8_t* rdram, uint32_t ucode_addr) {
    uint64_t start = svcGetSystemTick();
    audio_running = true;
    RspExitReason ret;
    if (audio_off) {
        ret = RspExitReason::Broke;
    } else if (audio_lle_only) {
        ret = aspMain(rdram, ucode_addr);
    } else if (audio_compare_left > 0) {
        audio_compare_left--;
        ret = audio_hle_compare(rdram, audio_task_data, audio_task_size, aspMain, ucode_addr);
    } else {
        ret = audio_hle_run(rdram, audio_task_data, audio_task_size);
    }
    audio_running = false;
    audio_rsp_ticks += svcGetSystemTick() - start;
    return ret;
}

static RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    if (task->t.type == M_AUDTASK) {
        audio_tasks_run++;
        audio_task_data = (uint32_t)task->t.data_ptr;
        audio_task_size = task->t.data_size;
        return timed_aspMain;
    }
    fprintf(stderr, "Unknown RSP task type %u\n", (unsigned)task->t.type);
    return nullptr;
}

// --- Input (input_3ds.cpp; the main thread publishes a snapshot each frame) -----------------------

static void poll_input() {}
static void set_rumble(int, bool) {} // no rumble motor on 3DS
static ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (controller_num == 0) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

static void message_box(const char* msg) {
    fprintf(stderr, "[runtime] %s\n", msg);
    fflush(stderr);
    log_ring_drain(); // the log is kept in memory (log_ring.c)
}

// Log uncaught C++ exceptions (stderr goes to sdmc:/3ds/psnap/runtime.log) before aborting.
static void on_terminate() {
    std::exception_ptr ex = std::current_exception();
    try {
        if (ex) std::rethrow_exception(ex);
        fprintf(stderr, "[terminate] called without an active exception\n");
    } catch (const std::exception& e) {
        fprintf(stderr, "[terminate] uncaught exception: %s\n", e.what());
    } catch (...) {
        fprintf(stderr, "[terminate] uncaught non-std exception\n");
    }
    fflush(stderr);
    log_ring_drain();
    abort();
}

// --- Boot entry point used by main.c ------------------------------------------------------------

static void register_overlays() {
    recomp::overlays::overlay_section_table_data_t sections {
        .code_sections = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };
    recomp::overlays::overlays_by_index_t overlays {
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };
    recomp::overlays::register_overlays(sections, overlays);
}

extern "C" SnapBootResult snap_boot(const char* rom_path, const char* data_path) {
    std::set_terminate(on_terminate);
    register_overlays();
    recomp3ds_boot_code_size = section_table[0].size; // .main only; the rest come by dmaLoadOverlay

    std::string compare_path = std::string(data_path) + "/audio_compare.txt";
    if (FILE* f = fopen(compare_path.c_str(), "r")) {
        char word[16] = {};
        if (fscanf(f, "%15s", word) == 1) {
            if (!strcmp(word, "lle")) audio_lle_only = true;
            else if (!strcmp(word, "off")) audio_off = true;
            else audio_compare_left = (unsigned)atoi(word);
        }
        fclose(f);
        fprintf(stderr, "[audio] development: %s\n", audio_lle_only ? "recompiled microcode only" : "comparing native with recompiled");
    }

    recomp3ds::BootConfig cfg{};
    cfg.rom_path = rom_path;
    cfg.data_path = data_path;
    cfg.game.internal_name = "POKEMON SNAP";
    cfg.game.display_name = "Pokemon Snap";
    cfg.game.game_id = u8"pokemonsnap"; // same save identity as Snap64Recomp
    cfg.game.mod_game_id = "pokemonsnap";
    cfg.game.save_type = recomp::SaveType::AllowAll; // boot probes FlashRAM before using it
    cfg.game.entrypoint_address = get_entrypoint_address();
    cfg.game.entrypoint = recomp_entrypoint;
    cfg.rsp_callbacks = { .get_rsp_microcode = get_rsp_microcode };
    cfg.renderer_callbacks = { .create_render_context = create_render_context };
    audio3ds::init();
    cfg.audio_callbacks = { .queue_samples = audio3ds::queue_samples, .get_frames_remaining = audio3ds::get_frames_remaining, .set_frequency = audio3ds::set_frequency };
    cfg.input_callbacks = { .poll_input = poll_input, .get_input = input3ds::get_input, .set_rumble = set_rumble, .get_connected_device_info = get_connected_device_info };
    cfg.error_handling_callbacks = { .message_box = message_box };

    switch (recomp3ds::boot(cfg)) {
        case recomp3ds::BootResult::Started:        return SNAP_BOOT_STARTED;
        case recomp3ds::BootResult::RomNotFound:    return SNAP_BOOT_ROM_NOT_FOUND;
        case recomp3ds::BootResult::RomWrongFormat: return SNAP_BOOT_ROM_WRONG_FORMAT;
        case recomp3ds::BootResult::OutOfMemory:    return SNAP_BOOT_OUT_OF_MEMORY;
    }
    return SNAP_BOOT_OUT_OF_MEMORY;
}

extern "C" void snap_set_paused(bool paused) {
    ultramodern::set_game_paused(paused);
    audio3ds::set_paused(paused);
}

extern "C" void snap_shutdown(void) {
    audio3ds::exit();
}

extern "C" void snap_get_stats(SnapStats* out) {
    out->display_lists = display_lists_seen.load();
    out->display_lists_done = display_lists_done.load();
    out->frames = frames_presented.load();
    out->audio_tasks = audio_tasks_run.load();
    out->audio_rsp_ms = (unsigned)(audio_rsp_ticks.load() / (SYSCLOCK_ARM11 / 1000));
    audio3ds::DebugStats a = audio3ds::debug_stats();
    out->audio_queued_frames = a.queued_frames;
    out->audio_dropped_frames = a.dropped_frames;
    out->audio_peak = a.peak;
    out->audio_playing = a.playing;
}

extern "C" void snap_audio_status(char* out, unsigned size) {
    audio3ds::status_line(out, size);
}

extern "C" void snap_debug_record_audio(unsigned frames) {
    audio3ds::record("sdmc:/3ds/psnap/audio_rec.raw", frames);
}

// --- Function-entry trace (N64Recomp trace_mode, see trace.h) -----------------------------------

extern "C" {
const char* volatile recomp_trace_ring[RECOMP_TRACE_RING];
volatile unsigned recomp_trace_pos;
}

// Sampling profiler (development): every ~1 ms a high-priority thread records which recompiled
// function was entered most recently, if the game ran anything since the last sample.
static std::mutex profile_mutex;
static std::unordered_map<const char*, unsigned> profile_counts;

extern "C" volatile int n64_gfx_phase;
static unsigned core_samples, gfx_phase_samples[8], audio_samples, game_samples;

static void profiler_thread(void*) {
    unsigned last_pos = 0;
    for (;;) {
        svcSleepThread(1000000);
        unsigned pos = recomp_trace_pos;
        {
            std::lock_guard lock{ profile_mutex };
            core_samples++;
            int phase = n64_gfx_phase;
            if (phase > 0 && phase < 8) gfx_phase_samples[phase]++;
            if (audio_running) audio_samples++;
            if (pos != last_pos) game_samples++;
        }
        if (pos == last_pos) continue;
        last_pos = pos;
        const char* name = recomp_trace_ring[(pos - 1) & (RECOMP_TRACE_RING - 1)];
        if (!name) continue;
        std::lock_guard lock{ profile_mutex };
        profile_counts[name]++;
    }
}

extern "C" void snap_profile_start(void) {
    threadCreate(profiler_thread, nullptr, 8 * 1024, 0x18, -2, true);
}

extern "C" void snap_profile_dump(void) {
    std::vector<std::pair<unsigned, const char*>> top;
    {
        std::lock_guard lock{ profile_mutex };
        for (auto& [name, count] : profile_counts) top.push_back({ count, name });
        profile_counts.clear();
    }
    std::sort(top.rbegin(), top.rend());
    {
        std::lock_guard lock{ profile_mutex };
        if (core_samples) {
            auto pct = [](unsigned n) { return n * 100 / core_samples; };
            fprintf(stderr, "[cores] %u samples, %% of time: gfx cmd %u vtx %u tri %u tex-lookup %u tex-decode %u draw-state %u | audio %u | game code active %u\n",
                core_samples, pct(gfx_phase_samples[1]), pct(gfx_phase_samples[2]), pct(gfx_phase_samples[3]),
                pct(gfx_phase_samples[4]), pct(gfx_phase_samples[5]), pct(gfx_phase_samples[6]), pct(audio_samples), pct(game_samples));
        }
        core_samples = audio_samples = game_samples = 0;
        for (unsigned& n : gfx_phase_samples) n = 0;
    }
    fprintf(stderr, "[profile]");
    for (size_t i = 0; i < top.size() && i < 12; i++) fprintf(stderr, " %s:%u", top[i].second, top[i].first);
    fprintf(stderr, "\n");
}

extern "C" uint8_t* snap_debug_rdram(void) { return recomp3ds::get_rdram(); }

extern "C" std::atomic<unsigned> ultramodern_thread_resumes; // runtime/ultramodern/threads.cpp
extern "C" unsigned snap_thread_resumes(void) { return ultramodern_thread_resumes.load(); }

// Average latency of a game thread switch (signal to running) since the last call, in microseconds.
extern "C" std::atomic<uint64_t> ultramodern_switch_ticks;
extern "C" std::atomic<unsigned> ultramodern_switch_count;
extern "C" unsigned snap_switch_latency_us(void) {
    uint64_t ticks = ultramodern_switch_ticks.exchange(0);
    unsigned n = ultramodern_switch_count.exchange(0);
    return n ? (unsigned)(ticks / n / (SYSCLOCK_ARM11 / 1000000)) : 0;
}

extern "C" unsigned int snap_debug_peek(unsigned int address) {
    uint8_t* rdram = recomp3ds::get_rdram();
    if (!rdram) return 0;
    return *reinterpret_cast<uint32_t*>(rdram + ((address & 0x00FFFFFF) & ~3u));
}

extern "C" void snap_trace_dump(void) {
    unsigned pos = recomp_trace_pos;
    fprintf(stderr, "[trace] %u calls, newest last:", pos);
    for (unsigned i = RECOMP_TRACE_RING - 64; i < RECOMP_TRACE_RING; i++) {
        const char* name = recomp_trace_ring[(pos + i) & (RECOMP_TRACE_RING - 1)];
        if (name) fprintf(stderr, " %s", name);
    }
    fprintf(stderr, "\n");
}

extern "C" void snap_trace_unique(unsigned count) {
    static const char* seen[4096];
    int n = 0;
    unsigned pos = recomp_trace_pos;
    if (count > RECOMP_TRACE_RING) count = RECOMP_TRACE_RING;
    fprintf(stderr, "[traceset] %u calls:", count);
    for (unsigned i = 0; i < count; i++) {
        const char* name = recomp_trace_ring[(pos - count + i) & (RECOMP_TRACE_RING - 1)];
        if (!name) continue;
        int j = 0;
        while (j < n && seen[j] != name) j++;
        if (j < n) continue;
        if (n < 4096) seen[n++] = name;
        fprintf(stderr, " %s", name);
    }
    fprintf(stderr, "\n");
}
