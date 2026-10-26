// 3DS replacement for N64ModernRuntime's librecomp/src/recomp.cpp.
// Keeps the CPU-context helpers and game boot sequence; drops the mod loader, live recompiler (JIT),
// launcher/ROM-selection UI and virtual-memory RDRAM reservation, none of which exist on 3DS.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/error_handling.hpp"

#include "recomp_3ds.hpp"

// Globals other librecomp files reference (pi.cpp, flash.cpp, events).
std::filesystem::path config_path;
std::atomic_bool exited = false;
moodycamel::LightweightSemaphore graphics_shutdown_ready;

static recomp::SaveType save_type = recomp::SaveType::None;
static std::optional<recomp::GameEntry> current_game;
static std::atomic_bool game_started = false;

// --- CPU context helpers called by recompiled code ---------------------------------------------

extern "C" void osGetMemSize_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 8 * 1024 * 1024;
}

enum class StatusReg {
    FR = 0x04000000,
};

extern "C" void cop0_status_write(recomp_context* ctx, gpr value) {
    uint32_t old_sr = ctx->status_reg;
    uint32_t new_sr = (uint32_t)value;
    uint32_t changed = old_sr ^ new_sr;

    // Check if the FR bit changed
    if (changed & (uint32_t)StatusReg::FR) {
        if (new_sr & (uint32_t)StatusReg::FR) {
            // FR = 1, odd single floats point to their own registers
            ctx->f_odd = &ctx->f1.u32l;
            ctx->mips3_float_mode = true;
        }
        else {
            // FR = 0, odd single floats point to the upper half of the previous register
            ctx->f_odd = &ctx->f0.u32h;
            ctx->mips3_float_mode = false;
        }
        changed &= ~(uint32_t)StatusReg::FR;
    }

    if (changed) {
        printf("Unhandled status register bits changed: 0x%08X\n", (unsigned)changed);
        ULTRAMODERN_QUICK_EXIT();
    }

    ctx->status_reg = new_sr;
}

extern "C" gpr cop0_status_read(recomp_context* ctx) {
    return (gpr)(int32_t)ctx->status_reg;
}

extern "C" void switch_error(const char* func, uint32_t vram, uint32_t jtbl) {
    printf("Switch-case out of bounds in %s at 0x%08X for jump table at 0x%08X\n", func, (unsigned)vram, (unsigned)jtbl);
    ULTRAMODERN_QUICK_EXIT();
}

extern "C" void do_break(uint32_t vram) {
    printf("Encountered break at original vram 0x%08X\n", (unsigned)vram);
    ULTRAMODERN_QUICK_EXIT();
}

void run_thread_function(uint8_t* rdram, uint64_t addr, uint64_t sp, uint64_t arg) {
    recomp_context ctx{};
    ctx.r29 = sp;
    ctx.r4 = arg;
    ctx.mips3_float_mode = 0;
    ctx.f_odd = &ctx.f0.u32h;

    if (current_game->thread_create_callback != nullptr) {
        current_game->thread_create_callback(rdram, &ctx);
    }

    recomp_func_t* func = get_function(addr);
    func(rdram, &ctx);
}

// --- Game state queried by the rest of the runtime ---------------------------------------------

std::u8string recomp::current_game_id() {
    return current_game->game_id;
}

std::string recomp::current_mod_game_id() {
    return current_game->mod_game_id;
}

bool ultramodern::is_game_started() {
    return game_started.load();
}

void ultramodern::quit() {
    exited.store(true);
}

recomp::SaveType recomp::get_save_type() {
    return save_type;
}

bool recomp::eeprom_allowed() {
    return save_type == SaveType::Eep4k || save_type == SaveType::Eep16k || save_type == SaveType::AllowAll;
}

bool recomp::sram_allowed() {
    return save_type == SaveType::Sram || save_type == SaveType::AllowAll;
}

bool recomp::flashram_allowed() {
    return save_type == SaveType::Flashram || save_type == SaveType::AllowAll;
}

std::filesystem::path recomp::get_config_path() {
    return config_path;
}

// --- Boot ---------------------------------------------------------------------------------------

// Code registered at boot: the sections in this much ROM from 0x1000, placed linearly from the
// entry point (librecomp registers 1 MB). Pokemon Snap sets its .main size: the other sections in
// that range are overlays its dmaLoadOverlay places elsewhere (source/port/snap_game.cpp).
uint32_t recomp3ds_boot_code_size = 1024 * 1024;

// Same as librecomp's init(): load the first 1 MB like IPL3 does and set the IPL3 globals.
static void init(uint8_t* rdram, recomp_context* ctx, gpr entrypoint) {
    recomp::overlays::init_overlays();

    load_overlays(0x1000, (int32_t)entrypoint, recomp3ds_boot_code_size);

    // Initial 1MB DMA (rom address 0x1000 = physical address 0x10001000)
    recomp::do_rom_read(rdram, entrypoint, 0x10001000, 0x100000);

    ctx->f_odd = &ctx->f0.u32h;
    ctx->mips3_float_mode = false;

    constexpr int32_t osTvType = 0x80000300;
    constexpr int32_t osRomBase = 0x80000308;
    constexpr int32_t osResetType = 0x8000030c;
    constexpr int32_t osMemSize = 0x80000318;
    MEM_W(osTvType, 0) = 1; // NTSC
    MEM_W(osRomBase, 0) = 0xB0000000u; // standard rom base
    MEM_W(osResetType, 0) = 0; // cold reset
    MEM_W(osMemSize, 0) = 8 * 1024 * 1024; // 8MB
}

static bool read_rom_header(const std::filesystem::path& path, std::vector<uint8_t>& out) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return false;
    }
    out.resize(0x40);
    file.read(reinterpret_cast<char*>(out.data()), out.size());
    return file.good();
}

static uint8_t* rdram_buffer = nullptr;

// Game-visible RDRAM (8 MB, as osGetMemSize reports) + 1 MB for the runtime's own area at 0x80800000.
constexpr size_t rdram_alloc_size = 9 * 1024 * 1024;
static_assert((uint32_t)recomp::patch_rdram_start - 0x80000000u < rdram_alloc_size);

uint8_t* recomp3ds::get_rdram() {
    return rdram_buffer;
}

recomp3ds::BootResult recomp3ds::boot(const BootConfig& cfg) {
    config_path = cfg.data_path;
    current_game = cfg.game;
    save_type = cfg.game.save_type;

    std::vector<uint8_t> header;
    if (!read_rom_header(cfg.rom_path, header)) {
        return BootResult::RomNotFound;
    }
    // Only big-endian .z64 dumps are supported (check the N64 header's first word).
    if (header[0] != 0x80 || header[1] != 0x37) {
        return BootResult::RomWrongFormat;
    }
    // The ROM stays on the SD card; PI DMA streams from it (32 MB won't fit in app memory).
    if (!recomp::set_rom_file(cfg.rom_path)) {
        return BootResult::RomNotFound;
    }

    recomp::rsp::set_callbacks(cfg.rsp_callbacks);
    static const ultramodern::rsp::callbacks_t ultramodern_rsp_callbacks {
        .init = recomp::rsp::constants_init,
        .run_task = recomp::rsp::run_task,
    };
    ultramodern::set_callbacks(ultramodern_rsp_callbacks, cfg.renderer_callbacks, cfg.audio_callbacks,
        cfg.input_callbacks, cfg.gfx_callbacks, cfg.events_callbacks, cfg.error_handling_callbacks, cfg.threads_callbacks);
    ultramodern::set_message_queue_control(cfg.message_queue_control);

    // 3DS has no mmap/guard pages: RDRAM is a plain zeroed buffer, so out-of-range accesses corrupt
    // memory instead of faulting. PC maps 512 MB; the runtime itself stores data past the game's
    // 8 MB (PI handles at recomp::cart_handle = 0x80800000, patch data from 0x80801000), so cover that too.
    rdram_buffer = static_cast<uint8_t*>(aligned_alloc(0x1000, rdram_alloc_size));
    if (rdram_buffer == nullptr) {
        return BootResult::OutOfMemory;
    }
    memset(rdram_buffer, 0, rdram_alloc_size);

    ultramodern::CoreThread game_thread{ultramodern::core::game, 128 * 1024, [](uint8_t* rdram) {
        // This thread becomes the game's boot thread, so it gets game-thread priority (see threads.cpp).
        ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::High);
        fprintf(stderr, "[boot] preinit (events, timers, audio, thread cleanup)\n");
        ultramodern::preinit(rdram, nullptr);

        fprintf(stderr, "[boot] loading first 1 MB and overlays\n");
        recomp_context context{};
        init(rdram, &context, current_game->entrypoint_address);
        if (current_game->on_init_callback) {
            current_game->on_init_callback(rdram, &context);
        }
        fprintf(stderr, "[boot] init saving\n");
        ultramodern::init_saving(rdram);
        game_started.store(true);

        fprintf(stderr, "[boot] entering game entrypoint\n");
        try {
            current_game->entrypoint(rdram, &context);
        } catch (ultramodern::thread_terminated&) {
        }
    }, rdram_buffer};
    game_thread.detach();

    return BootResult::Started;
}
