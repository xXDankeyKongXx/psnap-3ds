#ifndef INPUT_3DS_HPP
#define INPUT_3DS_HPP

#include <cstdint>

namespace input3ds {
    // ultramodern input callback: N64 buttons + stick (-1..1, y up) for controller 0.
    bool get_input(int controller_num, uint16_t* buttons, float* x, float* y);
}

#endif
