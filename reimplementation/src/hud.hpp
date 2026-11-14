#pragma once

#include "swiv3d/mission.hpp"

namespace swiv3d::detail {
enum class MapMarkers { normal, debug };
void draw_debug_coordinates(MissionFrame& frame, const Archive& globals, const MissionState& state,
                            const std::vector<std::uint8_t>& alpha, int counter);

void draw_initial_hud(MissionFrame& frame, const Archive& globals, const FrontendState& request,
                      const MissionState& state, const std::vector<std::uint8_t>& map,
                      const std::vector<std::uint8_t>& alpha, const std::vector<std::uint8_t>& palette,
                      MapMarkers markers = MapMarkers::normal);

}
