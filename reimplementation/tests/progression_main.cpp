#include "../src/input_script.hpp"
#include "../src/checkpoint.hpp"
#include "../src/profile_file.hpp"

#include <iostream>

namespace {
constexpr std::array<const char*, 17> missions{{
    "TERRAN00.slv", "TERRAN01.slv", "TERRAN02.slv", "TERRAN03.slv", "TERRAN04.slv",
    "TERRAN05.slv", "TERRAN06.slv", "TERRANBS.slv", "ARCTIC01.slv", "ARCTIC02.slv",
    "ARCTIC03.slv", "LUNAR01.slv", "MARS01.slv", "MARS02.slv", "MARS03.slv", "MARS04.slv", "MARS05.slv"
}};

void skip_briefings(swiv3d::Frontend& frontend) {
    for (int count = 0; count < 8 && frontend.state().phase == swiv3d::FrontendPhase::briefing; ++count) {
        frontend.press(0x01);
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 4) { throw std::runtime_error("Expected GAME FRONTEND OUTPUT"); }
        const std::filesystem::path output(argv[3]);
        if (std::filesystem::exists(output)) { throw std::runtime_error("Progression output must be fresh"); }
        std::filesystem::create_directories(output);
        auto session = swiv3d::detail::start_mission(argv[1], argv[2]);
        auto& frontend = *session.frontend;
        std::ofstream trace(output / "sequence.csv");
        trace << "level,file,campaign,vehicle,water,cursor\n";
        for (std::size_t ordinal = 0; ordinal < missions.size(); ++ordinal) {
            const auto request = frontend.state();
            if (request.phase != swiv3d::FrontendPhase::mission_request || request.level != ordinal || request.level_file != missions[ordinal]) {
                throw std::runtime_error("Campaign skipped or repeated a mission");
            }
            trace << request.level << ',' << request.level_file << ',' << request.craft << ','
                  << unsigned(request.mission_profile[44]) << ',' << request.water << ',' << request.sequence_offset << '\n';
            auto path = session.game / request.level_file;
            path.replace_extension(".SLV");
            swiv3d::Mission mission(swiv3d::Archive::open(path), request, frontend.configuration(), &session.globals);
            swiv3d::detail::checkpoint(mission, mission.render(session.globals), output / ("level-" + std::to_string(ordinal)));
            // Exercise the campaign's successful-result contract, not a played mission.
            swiv3d::MissionResult result{swiv3d::MissionOutcome::completed};
            result.profile = request.mission_profile;
            result.landscape_clock = mission.state().landscape_clock;
            frontend.finish_mission(result);
            skip_briefings(frontend);
        }
        if (frontend.state().phase != swiv3d::FrontendPhase::main_menu) { throw std::runtime_error("Campaign did not terminate"); }
        frontend.press(0x1c);
        if (frontend.state().phase != swiv3d::FrontendPhase::level_selection) { throw std::runtime_error("Completed pilot lost retry menu"); }
        frontend.press(0x1c);
        skip_briefings(frontend);
        if (frontend.state().level != 0 || frontend.state().level_file != missions.front()) { throw std::runtime_error("Completed pilot could not retry"); }
        swiv3d::MissionResult failed{swiv3d::MissionOutcome::failed};
        const auto profile = output / "completed-pilot.cfg";
        swiv3d::detail::save_profile(profile, frontend.finish_mission(failed));
        auto reloaded = swiv3d::detail::start_mission(argv[1], argv[2], profile);
        if (reloaded.frontend->state().level != 0) { throw std::runtime_error("Completed-pilot bootstrap did not select its first retry"); }
        std::cout << "All 17 campaign requests, initial frames, ending, retry and profile reload passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
