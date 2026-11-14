#pragma once

#include "swiv3d/mission.hpp"

namespace swiv3d::detail {

void queue_ground_effect(GroundEffects& effects, const std::array<std::int32_t, 3>& position,
                         GroundEffectKind kind, std::int32_t lifetime);
void advance_ground_effects(GroundEffects& effects);
void apply_ground_effects(const GroundEffects& effects, int x, int z, int& height, int& shade, int& texture);

}
