#include <cstdio>
// 3DS controls -> N64 controller for ultramodern's input callbacks.
// The main thread publishes a snapshot each frame (snap_input_update); the game thread reads it
// from get_input. Mapping is documented in docs/CONTROLS.md.

#include <3ds.h>

#include <algorithm>
#include <atomic>

#include "input_3ds.hpp"
#include "gyro.h"
#include "snap_game.h"

namespace {

// libultra button bits
constexpr uint16_t N64_A = 0x8000, N64_B = 0x4000, N64_Z = 0x2000, N64_START = 0x1000;
constexpr uint16_t N64_DU = 0x0800, N64_DD = 0x0400, N64_DL = 0x0200, N64_DR = 0x0100;
constexpr uint16_t N64_L = 0x0020, N64_R = 0x0010;
constexpr uint16_t N64_CU = 0x0008, N64_CD = 0x0004, N64_CL = 0x0002, N64_CR = 0x0001;

struct Mapping {
    u32 key;
    uint16_t n64;
};

// Pokemon Snap: Circle Pad aims, A apple / OK, B Pester Ball / back, Z (hold) zooms - and A takes
// the picture while zoomed - R dash, C-Down Poke Flute, Start pause. Z is on L as well as ZL/ZR (the
// Old 3DS has no ZL/ZR; Snap doesn't use the N64's L). Y is C-Down, X C-Up, the C-Stick the C buttons.
constexpr Mapping mappings[] = {
    { KEY_A, N64_A },
    { KEY_B, N64_B },
    { KEY_L, N64_Z },
    { KEY_ZL, N64_Z },
    { KEY_ZR, N64_Z },
    { KEY_START, N64_START },
    { KEY_SELECT, N64_START },
    { KEY_R, N64_R },
    { KEY_DUP, N64_DU },
    { KEY_DDOWN, N64_DD },
    { KEY_DLEFT, N64_DL },
    { KEY_DRIGHT, N64_DR },
    { KEY_X, N64_CU },
    { KEY_Y, N64_CD },
    { KEY_CSTICK_UP, N64_CU },
    { KEY_CSTICK_DOWN, N64_CD },
    { KEY_CSTICK_LEFT, N64_CL },
    { KEY_CSTICK_RIGHT, N64_CR },
};

// Circle Pad reports roughly +/-156 at full tilt.
constexpr float circle_pad_max = 150.0f;

std::atomic<uint16_t> n64_buttons{ 0 };
std::atomic<int16_t> pad_x{ 0 }, pad_y{ 0 };

} // namespace

extern "C" void snap_input_update(unsigned keys_held, int circle_x, int circle_y, bool game_paused) {
    if (game_paused) {
        // The game is frozen; buttons belong to the bottom-screen UI.
        n64_buttons = 0;
        pad_x = 0;
        pad_y = 0;
        return;
    }
    uint16_t buttons = 0;
    for (const Mapping& m : mappings) {
        if (keys_held & m.key) buttons |= m.n64;
    }
    static uint16_t logged_buttons;
    if ((buttons & N64_B) && !(logged_buttons & N64_B)) fprintf(stderr, "[input] N64 B down\n");
    logged_buttons = buttons;
    n64_buttons = buttons;
    pad_x = (int16_t)circle_x;
    pad_y = (int16_t)circle_y;
}

bool input3ds::get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0) return false;
    *buttons = n64_buttons;
    *x = std::clamp(pad_x / circle_pad_max, -1.0f, 1.0f);
    *y = std::clamp(pad_y / circle_pad_max, -1.0f, 1.0f);
    gyro_take(x, y);
    return true;
}
