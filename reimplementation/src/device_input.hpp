#pragma once

#include "swiv3d/controller.hpp"

namespace swiv3d::host {

enum class InputFocus { background, foreground };
enum class PointerMode { free, relative };

class DeviceInputHost {
public:
    DeviceInput poll(InputFocus focus, PointerMode pointer);

private:
    int controller_ = -1;
    std::uint64_t next_probe_ = 0;
    bool mouse_control_ = false;
    int mouse_x_ = 0, mouse_y_ = 0;
};

}
