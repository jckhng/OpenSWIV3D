#include "scenario.hpp"
#include "../src/checkpoint.hpp"
#include "../src/profile_file.hpp"

#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc != 6) { throw std::runtime_error("Expected GAME FRONTEND MOVIE LOADOUT OUTPUT"); }
        const std::filesystem::path output(argv[5]);
        if (std::filesystem::exists(output)) { throw std::runtime_error("Session output must be fresh"); }
        std::filesystem::create_directories(output);
        const auto profile = output / "SWIV.CFG";
        auto session = swiv3d::detail::start_mission(argv[1], argv[2], profile);
        scenario::loadout(session, argv[4]);
        std::ifstream input(argv[3]);
        int ordinal = 0;
        auto frame = swiv3d::detail::render_session(session);
        for (const auto& segment : swiv3d::detail::read_flight(input)) {
            for (unsigned tick = 0; tick < segment.updates; ++tick) {
                swiv3d::detail::advance_session(session, segment.keys);
                frame = swiv3d::detail::render_session(session);
            }
            const auto directory = output / ("step-" + std::to_string(ordinal++));
            swiv3d::detail::checkpoint(*session.mission, frame, directory);
            const auto state = session.frontend->state();
            std::ofstream scene(directory / "scene.json");
            scene << "{\"phase\":\"" << swiv3d::phase_name(state.phase) << "\",\"level\":" << state.level
                  << ",\"level_file\":\"" << state.level_file << "\"}\n";
        }
        swiv3d::detail::abort_mission(session);
        if (!std::filesystem::exists(profile)) { throw std::runtime_error("Session did not persist a profile"); }
        auto reloaded = swiv3d::detail::start_mission(argv[1], argv[2], profile);
        swiv3d::detail::checkpoint(*reloaded.mission, swiv3d::detail::render_session(reloaded), output / "reloaded");
        std::ofstream result(output / "reloaded/campaign.json");
        const auto state = reloaded.frontend->state();
        result << "{\"level\":" << state.level << ",\"level_file\":\"" << state.level_file << "\"}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
