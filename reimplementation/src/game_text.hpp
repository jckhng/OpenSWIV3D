#pragma once

#include "swiv3d/mission.hpp"

namespace swiv3d::detail {

enum class MessageMotion { flying = 0, held = 10, notice = 11 };
void cue_message(MissionState& state, std::uint32_t group, std::uint32_t id, int colour,
                 MessageMotion motion = MessageMotion::flying);
void advance_messages(MissionState& state);
void draw_messages(MissionFrame& frame, const MissionState& state, const FrontendState& request,
                   const Archive& globals, const std::vector<std::uint8_t>& palette);

}
