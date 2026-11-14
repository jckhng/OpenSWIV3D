#pragma once

#include "swiv3d/mission.hpp"

namespace swiv3d::detail {

bool save_screen_grab(const std::filesystem::path& path, const MissionFrame& frame);

}
