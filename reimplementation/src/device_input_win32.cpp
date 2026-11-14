#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <Xinput.h>

#include "device_input.hpp"

#include <algorithm>

namespace swiv3d::host {
namespace {
constexpr std::uint64_t probe_interval_ms = 1000;
constexpr unsigned controller_count = 4;

int stick(int value, int dead_zone) {
    if (value >= -dead_zone && value <= dead_zone) { return 0; }
    const int extent = value < 0 ? 32768 : 32767;
    const int magnitude = value < 0 ? -value : value;
    const int normalized = (magnitude - dead_zone) * 128 / (extent - dead_zone);
    return value < 0 ? -normalized : normalized;
}
}

DeviceInput DeviceInputHost::poll(InputFocus focus, PointerMode pointer_mode) {
    const bool focused = focus == InputFocus::foreground;
    const bool mouse_control = pointer_mode == PointerMode::relative;
    DeviceInput result;
    XINPUT_STATE state{};
    if (controller_ >= 0 && XInputGetState(controller_,&state) != ERROR_SUCCESS) {
        controller_ = -1;
        next_probe_ = 0;
    }
    const auto now = GetTickCount64();
    if (controller_ < 0 && now >= next_probe_) {
        for (unsigned index = 0; index < controller_count; ++index) {
            if (XInputGetState(index,&state) == ERROR_SUCCESS) { controller_ = static_cast<int>(index); break; }
        }
        next_probe_ = now + probe_interval_ms;
    }
    result.joystick_connected = controller_ >= 0;
    if (focused && result.joystick_connected) {
        const auto& pad = state.Gamepad;
        result.joystick = {stick(pad.sThumbLX,XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE),
                           stick(pad.sThumbLY,XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE),
                           stick(pad.sThumbRX,XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE)};
        result.joystick_buttons = {{(pad.wButtons & XINPUT_GAMEPAD_A) != 0 || pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD,
                                    (pad.wButtons & XINPUT_GAMEPAD_B) != 0 || pad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD,
                                    (pad.wButtons & XINPUT_GAMEPAD_X) != 0,
                                    (pad.wButtons & XINPUT_GAMEPAD_Y) != 0}};
    }
    POINT point{};
    if (focused && mouse_control && GetCursorPos(&point)) {
        if (mouse_control_) {
            result.mouse = {std::clamp(static_cast<int>(point.x) - mouse_x_,-128,128),
                            std::clamp(mouse_y_ - static_cast<int>(point.y),-128,128),0};
        }
        mouse_control_ = false;
        RECT area{};
        const auto handle = GetForegroundWindow();
        if (GetClientRect(handle,&area)) {
            POINT centre{area.right / 2,area.bottom / 2};
            if (ClientToScreen(handle,&centre) && SetCursorPos(centre.x,centre.y)) {
                mouse_x_ = centre.x; mouse_y_ = centre.y; mouse_control_ = true;
            }
        }
    } else { mouse_control_ = false; }
    if (focused) {
        result.mouse_buttons = {{(GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0,
                                 (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0,
                                 (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0}};
    }
    return result;
}
}
