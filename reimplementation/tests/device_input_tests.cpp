#include <windows.h>
#include <Xinput.h>
#include "../src/device_input.hpp"

#include <iostream>
#include <stdexcept>

namespace {
int connected = -1;
unsigned polls = 0;
std::uint64_t clock_ms = 0;
XINPUT_STATE sample{};
POINT pointer{100,100};
short buttons = 0;
void require(bool value,const char* message) {
    if (!value) { throw std::runtime_error(message); }
}
}

unsigned long XInputGetState(unsigned long index,XINPUT_STATE* state) {
    ++polls;
    if (static_cast<int>(index) != connected) { return 1167; }
    *state = sample; return ERROR_SUCCESS;
}
std::uint64_t GetTickCount64() { return clock_ms; }
HWND GetForegroundWindow() { return nullptr; }
BOOL GetCursorPos(POINT* point) { *point = pointer; return 1; }
BOOL GetClientRect(HWND,RECT* area) { *area = {0,0,960,720}; return 1; }
BOOL ClientToScreen(HWND,POINT*) { return 1; }
BOOL SetCursorPos(int x,int y) { pointer = {x,y}; return 1; }
short GetAsyncKeyState(int) { return buttons; }

int main() {
    try {
        using swiv3d::host::InputFocus;
        using swiv3d::host::PointerMode;
        swiv3d::host::DeviceInputHost input;
        require(!input.poll(InputFocus::foreground,PointerMode::free).joystick_connected && polls == 4,"Disconnected slots were not probed once");
        input.poll(InputFocus::foreground,PointerMode::free);
        require(polls == 4,"Disconnected slots were polled without backoff");
        clock_ms = 1000; connected = 2;
        sample.Gamepad = {XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_X,31,30,-32768,32767,8689,0};
        auto state = input.poll(InputFocus::foreground,PointerMode::free);
        require(state.joystick_connected && state.joystick == std::array<int,3>{-128,128,0} &&
                state.joystick_buttons == std::array<bool,4>{true,true,true,false},"XInput axis, trigger or button mapping differs");
        state = input.poll(InputFocus::background,PointerMode::free);
        require(state.joystick_connected && state.joystick == std::array<int,3>{} &&
                state.joystick_buttons == std::array<bool,4>{},"Focus loss retained controller input");
        connected = -1;
        state = input.poll(InputFocus::foreground,PointerMode::free);
        require(!state.joystick_connected && state.joystick == std::array<int,3>{},"Disconnect retained controller input");
        connected = 0; clock_ms += 1000;
        require(input.poll(InputFocus::foreground,PointerMode::free).joystick_connected,"Controller reconnection failed");
        buttons = static_cast<short>(0x8000);
        state = input.poll(InputFocus::foreground,PointerMode::relative);
        require(state.mouse == std::array<int,3>{} && state.mouse_buttons[0],"Pointer capture jumped on entry");
        pointer.x += 20; pointer.y -= 30;
        state = input.poll(InputFocus::foreground,PointerMode::relative);
        require(state.mouse == std::array<int,3>{20,30,0},"Relative pointer direction or recentering differs");
        state = input.poll(InputFocus::background,PointerMode::relative);
        require(state.mouse == std::array<int,3>{} && !state.mouse_buttons[0],"Focus loss retained mouse input");
        pointer = {0,0};
        require(input.poll(InputFocus::foreground,PointerMode::relative).mouse == std::array<int,3>{},"Focus return imported stale pointer movement");
        std::cout << "Injected XInput, hot-plug, focus and relative mouse checks passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
