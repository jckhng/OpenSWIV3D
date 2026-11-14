#pragma once

#include "swiv3d/mission.hpp"

namespace swiv3d::detail {
void apply_gamma(MissionFrame& frame, unsigned gamma);

void draw_menu(MissionFrame& frame, const Archive& globals, const std::vector<std::uint8_t>& alpha,
               const Frontend& frontend);

}
