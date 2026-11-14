#pragma once

#include "../src/input_script.hpp"

#include <fstream>
#include <stdexcept>

namespace scenario {

inline void loadout(swiv3d::detail::MissionSession& session, const std::filesystem::path& path) {
    std::ifstream fixture(path);
    std::string format, extra;
    int slot, ammo, level, health;
    if (!(fixture >> format >> slot >> ammo >> level >> health) || fixture >> extra || format != "swiv3d-loadout-v1" ||
        slot < -1 || slot >= 20 || ammo < 0 || ammo > 500 || level < 0 || level > 19 || health < 0 || health > 992) {
        throw std::runtime_error("Invalid controlled loadout");
    }
    auto request = session.frontend->state();
    if (slot >= 0) {
        request.mission_profile[slot * 2] = static_cast<std::uint8_t>(ammo);
        request.mission_profile[slot * 2 + 1] = static_cast<std::uint8_t>(ammo >> 8);
    }
    for (int i = 0; i < 4; ++i) {
        request.mission_profile[48 + i] = static_cast<std::uint8_t>(health >> (i * 8));
        request.mission_profile[52 + i] = static_cast<std::uint8_t>(level >> (i * 8));
    }
    auto level_path = session.game / request.level_file;
    level_path.replace_extension(".SLV");
    session.mission = std::make_unique<swiv3d::Mission>(swiv3d::Archive::open(level_path), request,
                                                     session.frontend->configuration(), &session.globals);
}

inline void campaign_prefix(swiv3d::detail::MissionSession& session, const std::filesystem::path& movie,
                            swiv3d::FrameCounterReset counter_reset = swiv3d::FrameCounterReset::render_end) {
    swiv3d::RenderTiming timing;
    timing.counter_reset = counter_reset;
    if (counter_reset == swiv3d::FrameCounterReset::event_loop_end) {
        swiv3d::detail::render_session(session,timing);
    }
    std::ifstream input(movie);
    for (const auto& segment : swiv3d::detail::read_flight(input)) {
        for (unsigned tick = 0; tick < segment.updates; ++tick) {
            swiv3d::detail::advance_session(session, segment.keys);
            if (counter_reset == swiv3d::FrameCounterReset::event_loop_end) {
                session.mission->finish_event_loop();
            }
            swiv3d::detail::render_session(session,timing);
        }
    }
    if (session.frontend->state().phase != swiv3d::FrontendPhase::mission_results || session.mission->state().finish_counter < 110) {
        throw std::runtime_error("Campaign prefix did not reach an acknowledged result boundary");
    }
    swiv3d::detail::finish_mission(session);
}

}
