#pragma once

#include "swiv3d/mission.hpp"

#include <functional>
#include <map>

namespace swiv3d::detail {

struct Camera {
    std::int32_t followed = no_entity;
    std::array<float, 3> target{}, position{};
    float tilt = 0;
    int yaw = 0;
    int pitch = 30;
    bool hold_position = false;
    std::array<int, 3> lock{0, 0, -1}, objective{0, 0, -1};
    std::array<int, 5> terrain_slopes{};
    std::uint32_t water_clock = 0;
};

MissionFrame render_mission(const MissionState& state, const FrontendState& request,
                           const std::vector<std::uint8_t>& bytes,
                           const std::map<std::uint32_t, Resource>& directory,
                           const std::function<std::int32_t(std::int32_t, std::int32_t)>& height,
                           const Archive* globals = nullptr,
                           Camera* camera = nullptr, RenderTiming timing = {},
                           const std::function<std::int32_t(std::int32_t, std::int32_t)>& surface_height = {});

}
