#pragma once

#include <array>
#include <cstdint>

namespace swiv3d {

enum class ControllerKind : std::uint32_t { keyboard = 1, mouse, joystick, gamepad, direct_input, creative };

struct DeviceCapabilities {
    bool mouse = true;
    bool joystick = false;
};

// Original AXIS coordinates. Positive Y moves forward. Buttons use the original
// mouse (0x90..0x92) and joystick (0x93..0x96) virtual scan codes.
struct DeviceInput {
    std::array<int, 3> mouse{};
    std::array<int, 3> joystick{};
    std::array<bool, 3> mouse_buttons{};
    std::array<bool, 4> joystick_buttons{};
    bool joystick_connected = false;
};

}
