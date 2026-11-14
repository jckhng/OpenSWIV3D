#pragma once
#include <cstdint>
struct XINPUT_GAMEPAD {
    std::uint16_t wButtons;
    std::uint8_t bLeftTrigger,bRightTrigger;
    std::int16_t sThumbLX,sThumbLY,sThumbRX,sThumbRY;
};
struct XINPUT_STATE { std::uint32_t dwPacketNumber; XINPUT_GAMEPAD Gamepad; };
constexpr int XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE = 7849;
constexpr int XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE = 8689;
constexpr int XINPUT_GAMEPAD_TRIGGER_THRESHOLD = 30;
constexpr int XINPUT_GAMEPAD_A = 0x1000, XINPUT_GAMEPAD_B = 0x2000;
constexpr int XINPUT_GAMEPAD_X = 0x4000, XINPUT_GAMEPAD_Y = 0x8000;
unsigned long XInputGetState(unsigned long,XINPUT_STATE*);
