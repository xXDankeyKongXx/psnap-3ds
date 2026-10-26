#ifndef __RECOMP_3DS_HPP__
#define __RECOMP_3DS_HPP__

// 3DS boot API: replaces recomp::start() from librecomp (see runtime/librecomp/recomp_3ds.cpp).

#include <filesystem>

#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultramodern.hpp"

namespace recomp3ds {
    struct BootConfig {
        std::filesystem::path rom_path;   // big-endian .z64 on the SD card
        std::filesystem::path data_path;  // saves go to <data_path>/saves/
        recomp::GameEntry game;
        recomp::rsp::callbacks_t rsp_callbacks;
        ultramodern::renderer::callbacks_t renderer_callbacks;
        ultramodern::audio_callbacks_t audio_callbacks;
        ultramodern::input::callbacks_t input_callbacks;
        ultramodern::gfx_callbacks_t gfx_callbacks;
        ultramodern::events::callbacks_t events_callbacks;
        ultramodern::error_handling::callbacks_t error_handling_callbacks;
        ultramodern::threads::callbacks_t threads_callbacks;
        ultramodern::MessageQueueControl message_queue_control;
    };

    enum class BootResult {
        Started,
        RomNotFound,
        RomWrongFormat,
        OutOfMemory,
    };

    // Loads the ROM, allocates RDRAM and starts the game on its own thread. Returns immediately.
    BootResult boot(const BootConfig& cfg);

    uint8_t* get_rdram();
}

#endif
