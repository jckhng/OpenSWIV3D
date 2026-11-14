#pragma once

#include "scenario.hpp"

namespace scenario {

// A declared unlocked checkpoint; earlier missions are not played by this fixture.
inline swiv3d::FrontendState campaign_checkpoint(swiv3d::detail::MissionSession& session, std::uint32_t level) {
    auto carried = session.frontend->state();
    auto configuration = session.frontend->configuration();
    constexpr std::size_t record_bytes = 128;
    constexpr unsigned pilot_kind = 20, pilot_identity = 1, checkpoint_base = 1000;
    const auto get = [&](std::size_t offset) {
        return unsigned(configuration.at(offset)) | unsigned(configuration.at(offset + 1)) << 8 |
               unsigned(configuration.at(offset + 2)) << 16 | unsigned(configuration.at(offset + 3)) << 24;
    };
    const auto put = [&](std::size_t offset, unsigned value) {
        for (int byte = 0; byte < 4; ++byte) { configuration.at(offset + byte) = static_cast<std::uint8_t>(value >> (byte * 8)); }
    };
    std::size_t checkpoint = configuration.size(), free = configuration.size();
    for (std::size_t offset = 0; offset < configuration.size(); offset += record_bytes) {
        if (get(offset) == UINT32_MAX && free == configuration.size()) { free = offset; }
        if (get(offset) != pilot_kind || get(offset + 4) != static_cast<unsigned>(carried.player)) { continue; }
        if (get(offset + 8) == pilot_identity) { put(offset + 0x20, level); }
        if (get(offset + 8) == checkpoint_base + carried.level) { checkpoint = offset; }
    }
    if (checkpoint == configuration.size() || free == configuration.size()) { throw std::runtime_error("No checkpoint fixture capacity"); }
    std::copy_n(configuration.begin() + checkpoint, record_bytes, configuration.begin() + free);
    put(free + 8, checkpoint_base + level);
    session.frontend = std::make_unique<swiv3d::Frontend>(session.globals, configuration);
    auto& frontend = *session.frontend;
    while (frontend.state().selection != 100 + carried.player) { frontend.press(0xd0); }
    frontend.press(0x1c); frontend.press(0x1c);
    for (int count = 0; count < 8 && frontend.state().phase == swiv3d::FrontendPhase::briefing; ++count) { frontend.press(0x01); }
    const auto chosen = frontend.state();
    if (chosen.phase != swiv3d::FrontendPhase::mission_request || chosen.level != level) { throw std::runtime_error("Checkpoint did not select its original campaign command"); }
    carried.level = chosen.level; carried.level_file = chosen.level_file; carried.sequence_offset = chosen.sequence_offset;
    carried.craft = chosen.craft; carried.water = chosen.water; carried.tracks = chosen.tracks; carried.mission_profile = chosen.mission_profile;
    return carried;
}

}
