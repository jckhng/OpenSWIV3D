#pragma once

#include "swiv3d/mission.hpp"

#include <functional>

namespace swiv3d::detail {

void advance_helicopter(EntityState& player, FlightState& flight, const KeyboardState& keys,
                        const std::function<std::int32_t(std::int32_t, std::int32_t)>& height);
void move_helicopter(EntityState& player, FlightState& flight,
                     const std::function<std::int32_t(std::int32_t, std::int32_t)>& height);
void move_aircraft(EntityState& actor, int clearance,
                   const std::function<std::int32_t(std::int32_t, std::int32_t)>& height);
void control_helicopter(EntityState& player, FlightState& flight, const KeyboardState& keys,
                        const EntityState& target);
void search_flight_target(MissionState& state);

}
