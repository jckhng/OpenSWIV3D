#include "../src/input_script.hpp"
#include "../src/checkpoint.hpp"

#include <algorithm>
#include <iostream>

namespace {
constexpr unsigned campaign_missions = 17;
constexpr unsigned render_stride = 4;
constexpr std::array<int,20> ammunition_limits{{100,60,30,40,20,20,20,10,20,10,40,30,15,40,20,5,20,10,20,500}};

void initial_loadout(swiv3d::detail::MissionSession& session, const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string format, extra;
    int health, primary;
    if (!(input >> format >> health >> primary) || format != "swiv3d-campaign-loadout-v1" ||
        health < 1 || health > 992 || primary < 0 || primary > 19) {
        throw std::runtime_error("Invalid campaign loadout header");
    }
    auto request = session.frontend->state();
    for (std::size_t slot = 0; slot < ammunition_limits.size(); ++slot) {
        int count;
        if (!(input >> count) || count < 0 || count > ammunition_limits[slot]) {
            throw std::runtime_error("Invalid campaign ammunition count");
        }
        request.mission_profile[slot * 2] = static_cast<std::uint8_t>(count);
        request.mission_profile[slot * 2 + 1] = static_cast<std::uint8_t>(count >> 8);
    }
    if (input >> extra) { throw std::runtime_error("Unexpected campaign loadout field"); }
    for (unsigned byte = 0; byte < 4; ++byte) {
        request.mission_profile[48 + byte] = static_cast<std::uint8_t>(health >> (byte * 8));
        request.mission_profile[52 + byte] = static_cast<std::uint8_t>(primary >> (byte * 8));
    }
    auto level = session.game / request.level_file;
    level.replace_extension(".SLV");
    session.mission = std::make_unique<swiv3d::Mission>(swiv3d::Archive::open(level),request,
                                                     session.frontend->configuration(),&session.globals);
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 7) { throw std::runtime_error("Expected GAME FRONTEND LOADOUT|--normal-start MOVIE MISSIONS OUTPUT"); }
        std::size_t consumed = 0;
        const auto expected = std::stoul(argv[5],&consumed);
        if (argv[5][consumed] != 0) { throw std::runtime_error("Invalid campaign mission count"); }
        if (expected == 0 || expected > campaign_missions) { throw std::runtime_error("Invalid campaign mission count"); }
        const std::filesystem::path output(argv[6]);
        if (std::filesystem::exists(output)) { throw std::runtime_error("Campaign replay output must be fresh"); }
        std::filesystem::create_directories(output);
        auto session = swiv3d::detail::start_mission(argv[1],argv[2]);
        const bool normal_start = std::string(argv[3]) == "--normal-start";
        if (!normal_start) { initial_loadout(session,argv[3]); }
        session.profile = output / "pilot.cfg";
        std::ifstream movie(argv[4]);
        const auto segments = swiv3d::detail::read_flight(movie);
        std::ofstream trace(output / "sequence.csv");
        trace << "event,level,update,mission_tick,health,objectives,completed\n";
        unsigned completed = 0, failures = 0, entries = 0;
        std::uint64_t updates = 0;
        int last_level = -1;
        std::uint32_t last_tick = 0;
        const auto entry = [&] {
            const auto frontend = session.frontend->state();
            if (normal_start && frontend.cheat_mode) { throw std::runtime_error("Normal-start replay entered Test Levels"); }
            const auto state = session.mission->state();
            if (frontend.phase != swiv3d::FrontendPhase::mission_request ||
                (int(frontend.level) == last_level && state.tick >= last_tick)) { return; }
            if (frontend.level != completed) { throw std::runtime_error("Campaign skipped or repeated a completed mission"); }
            const auto frame = swiv3d::detail::render_session(session);
            swiv3d::detail::checkpoint(*session.mission,frame,output / ("entry-" + std::to_string(entries++)));
            last_level = int(frontend.level);
            const auto player = std::find_if(state.entities.begin(),state.entities.end(),[&](const auto& actor) { return actor.slot == state.player; });
            trace << "entry," << frontend.level << ',' << updates << ',' << state.tick << ',' << player->words[2] << ',' << state.objectives << ',' << state.objectives_completed << '\n';
            std::cout << "Entered " << frontend.level << ' ' << frontend.level_file << " at update " << updates << '\n';
        };
        for (const auto& segment : segments) {
            for (unsigned tick = 0; tick < segment.updates; ++tick) {
                entry();
                const auto before = session.mission->state();
                last_tick = before.tick;
                swiv3d::detail::advance_session(session,segment.keys);
                if (updates % render_stride == 0) { swiv3d::detail::render_session(session); }
                ++updates;
                const auto state = session.mission->state();
                if (before.phase != swiv3d::MissionPhase::playing || state.phase == swiv3d::MissionPhase::playing) { continue; }
                const auto frontend = session.frontend->state();
                const auto player = std::find_if(state.entities.begin(),state.entities.end(),[&](const auto& actor) { return actor.slot == state.player; });
                const bool success = state.phase == swiv3d::MissionPhase::completed;
                trace << (success ? "completed" : "failed") << ',' << frontend.level << ',' << updates << ',' << state.tick << ','
                      << player->words[2] << ',' << state.objectives << ',' << state.objectives_completed << '\n';
                if (!success) { ++failures; continue; }
                if (frontend.level != completed++) { throw std::runtime_error("Invalid earned completion order"); }
            }
        }
        entry();
        const auto frontend = session.frontend->state();
        const auto final_state = session.mission->state();
        const auto final_player = std::find_if(final_state.entities.begin(),final_state.entities.end(),
            [&](const auto& actor) { return actor.slot == final_state.player; });
        const bool ending = frontend.phase == swiv3d::FrontendPhase::main_menu && frontend.level == 999;
        const bool accepted = completed == expected && (expected == campaign_missions ? ending : frontend.level == expected);
        std::ofstream summary(output / "summary.json");
        summary << "{\"updates\":" << updates << ",\"completed\":" << completed << ",\"failures\":" << failures
                << ",\"phase\":\"" << swiv3d::phase_name(frontend.phase) << "\",\"health\":" << final_player->words[2]
                << ",\"objectives\":" << final_state.objectives << ",\"objectives_completed\":" << final_state.objectives_completed
                << ",\"next_level\":" << frontend.level << ",\"all_missions_completed\":" << (ending && completed == campaign_missions ? "true" : "false")
                << ",\"normal_start\":" << (normal_start ? "true" : "false")
                << ",\"earned_campaign_completed\":" << (normal_start && ending && completed == campaign_missions ? "true" : "false")
                << ",\"accepted\":" << (accepted ? "true" : "false") << "}\n";
        if (!accepted) { throw std::runtime_error("Campaign did not reach the required earned-completion boundary"); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
