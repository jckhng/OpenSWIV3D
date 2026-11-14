#include "scenario.hpp"
#include "level_fixture.hpp"
#include "checkpoint_fixture.hpp"
#include "../src/checkpoint.hpp"
#include "../src/menu_render.hpp"

#include <algorithm>
#include <iostream>
#include <cctype>
#include <map>
#include <sstream>
#include <set>

int main(int argc, char** argv) {
    try {
        if (argc < 7) { throw std::runtime_error("Expected GAME FRONTEND PREFIX LOADOUT LEVEL OUTPUT [MOVIE] [--station|--burst|--intruder] [--craft=N]"); }
        const auto counter_reset = std::find(argv + 7, argv + argc, std::string("--event-loop")) != argv + argc ?
            swiv3d::FrameCounterReset::event_loop_end : swiv3d::FrameCounterReset::render_end;
        const bool retained_session = std::find(argv + 7,argv + argc,std::string("--session")) != argv + argc;
        auto session = swiv3d::detail::start_mission(argv[1], argv[2]);
        scenario::loadout(session, argv[4]);
        scenario::campaign_prefix(session, argv[3], counter_reset);
        auto request = session.frontend->state();
        request.level_file = argv[5];
        auto path = session.game / request.level_file;
        path.replace_extension(".SLV");
        auto level = swiv3d::Archive::open(path);
        bool handoff = false;
        int summary_updates = 0, results_updates = -1;
        struct RenderEvent { std::uint32_t tick, count; int rate; };
        std::vector<RenderEvent> renders;
        std::set<int> checkpoint_segments;
        const bool movie = argc >= 8 && std::string(argv[7]).rfind("--",0) != 0;
        const auto arrival_option = std::find_if(argv + 7,argv + argc,[](const char* value) {
            return std::string(value).rfind("--encounter-index=",0) == 0;
        });
        const unsigned arrival_index = arrival_option == argv + argc ? 0 :
            static_cast<unsigned>(std::stoul(std::string(*arrival_option).substr(18)));
        for (int index = movie ? 8 : 7; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option.rfind("--checkpoint-level=",0) == 0) {
                request = scenario::campaign_checkpoint(session,static_cast<unsigned>(std::stoul(option.substr(19))));
                auto expected = std::filesystem::path(request.level_file).stem().string();
                auto supplied = path.stem().string();
                for (auto& letter : expected) { letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter))); }
                if (expected != supplied) { throw std::runtime_error("Level resource differs from checkpoint sequence"); }
            }
            else if (option.rfind("--render-cadence=",0) == 0) {
                std::ifstream input(option.substr(17));
                std::string header;
                std::getline(input, header);
                if (header != "swiv3d-render-cadence-v1") { throw std::runtime_error("Invalid render cadence"); }
                RenderEvent event{};
                while (input >> event.tick >> event.count >> event.rate) {
                    if (event.tick != renders.size() || (!renders.empty() && (event.count < renders.back().count || event.count > renders.back().count + 1))) {
                        throw std::runtime_error("Unsupported render cadence");
                    }
                    renders.push_back(event);
                }
                if (!input.eof() || renders.empty()) { throw std::runtime_error("Incomplete render cadence"); }
            }
            else if (option == "--event-loop" || option == "--session" || option.rfind("--encounter-index=",0) == 0) { continue; }
            else if (option == "--handoff") { handoff = true; }
            else if (option.rfind("--checkpoint-segments=",0) == 0) {
                std::istringstream input(option.substr(22));
                std::string value;
                while (std::getline(input,value,',')) {
                    const int segment = std::stoi(value);
                    if (segment < 0 || !checkpoint_segments.insert(segment).second) { throw std::runtime_error("Invalid checkpoint segment"); }
                }
                if (checkpoint_segments.empty()) { throw std::runtime_error("Empty checkpoint segment list"); }
            }
            else if (option.rfind("--handoff-updates=",0) == 0) {
                const auto values = option.substr(18);
                const auto comma = values.find(',');
                if (comma == std::string::npos) { throw std::runtime_error("Expected summary,results update counts"); }
                summary_updates = std::stoi(values.substr(0,comma)); results_updates = std::stoi(values.substr(comma + 1));
                if (summary_updates < 0 || results_updates < 0) { throw std::runtime_error("Invalid results update history"); }
            }
            else if (option.rfind("--craft=",0) == 0) {
                const int craft = std::stoi(option.substr(8));
                if (craft < 0 || craft > 3) { throw std::runtime_error("Invalid player craft fixture"); }
                request.mission_profile[44] = static_cast<std::uint8_t>(craft);
            } else if (option.rfind("--campaign-set=",0) == 0) {
                request.craft = static_cast<unsigned>(std::stoi(option.substr(15)));
            } else if (option.rfind("--water=",0) == 0) {
                request.water = static_cast<unsigned>(std::stoi(option.substr(8)));
            } else if (option.rfind("--arsenal=",0) == 0) {
                std::ifstream input(option.substr(10));
                std::string format, extra;
                int slot, ammo, primary, health;
                if (!(input >> format >> slot >> ammo >> primary >> health) || input >> extra ||
                    format != "swiv3d-loadout-v1" || slot < -1 || slot >= 20 || ammo < 0 || ammo > 500 ||
                    primary < 0 || primary > 19 || health < 0 || health > 992) {
                    throw std::runtime_error("Invalid mission inventory fixture");
                }
                std::fill_n(request.mission_profile.begin(),40,0);
                if (slot >= 0) {
                    request.mission_profile[slot * 2] = static_cast<std::uint8_t>(ammo);
                    request.mission_profile[slot * 2 + 1] = static_cast<std::uint8_t>(ammo >> 8);
                }
                for (int i = 0; i < 4; ++i) {
                    request.mission_profile[48 + i] = static_cast<std::uint8_t>(health >> (i * 8));
                    request.mission_profile[52 + i] = static_cast<std::uint8_t>(primary >> (i * 8));
                }
            } else if (option.rfind("--encounter=",0) == 0) {
                const std::map<std::string,scenario::Arrival> arrivals{{"boss2",scenario::Arrival::boss2},{"pyramid",scenario::Arrival::pyramid},
                    {"eye",scenario::Arrival::eye},{"mars-face",scenario::Arrival::mars_face},{"ground-exit",scenario::Arrival::ground_exit},{"teleport",scenario::Arrival::teleport}};
                level = scenario::arrival_start(level,arrivals.at(option.substr(12)),arrival_index);
            } else if (option == "--station" || option == "--burst" || option == "--intruder" || option == "--ski-station" || option == "--boss") {
                level = scenario::arrival_start(level,option == "--boss" ? scenario::Arrival::boss : option == "--station" ? scenario::Arrival::station :
                    option == "--burst" ? scenario::Arrival::burst_factory : option == "--ski-station" ? scenario::Arrival::ski_station : scenario::Arrival::intruder_factory,arrival_index);
            } else { throw std::runtime_error("Unknown later-level fixture option"); }
        }
        const bool measured = counter_reset == swiv3d::FrameCounterReset::event_loop_end;
        if (measured && renders.empty()) { throw std::runtime_error("Measured event loop requires render cadence"); }
        session.mission = std::make_unique<swiv3d::Mission>(level,request,session.frontend->configuration(),&session.globals);
        auto& mission = *session.mission;
        const std::filesystem::path output(argv[6]);
        if (std::filesystem::exists(output)) { throw std::runtime_error("Later mission output must be fresh"); }
        std::filesystem::create_directories(output);
        const auto save_checkpoint = [&](const swiv3d::MissionFrame& captured, const std::filesystem::path& folder) {
            swiv3d::detail::checkpoint(mission,captured,folder);
            std::ofstream palette(folder / "palette.bin",std::ios::binary);
            palette.write(reinterpret_cast<const char*>(captured.palette.data()),captured.palette.size());
            std::ofstream camera(folder / "view-transform.bin",std::ios::binary);
            camera.write(reinterpret_cast<const char*>(captured.view_transform.data()),sizeof(captured.view_transform));
            if (!palette || !camera) { throw std::runtime_error("Cannot write later mission presentation state"); }
        };
        save_checkpoint(mission.first_frame(session.globals),output / "entry");
        swiv3d::RenderTiming timing;
        timing.counter_reset = counter_reset;
        if (measured) { timing.frames_per_second = renders.front().rate; }
        auto frame = mission.render(session.globals,timing);
        save_checkpoint(frame,output / "dispatch");
        std::ofstream trace;
        if (measured) {
            trace.open(output / "updates.csv");
            trace << "tick,game_counter,health_bar\n";
        }
        const auto trace_dispatch = [&] {
            if (measured) {
                const auto state = mission.state();
                trace << state.tick << ',' << state.game_counter << ',' << state.health_bar << '\n';
            }
        };
        trace_dispatch();
        std::vector<swiv3d::detail::FlightSegment> route{{100,{}},{200,{}}};
        if (movie) {
            std::ifstream input(argv[7]);
            route = swiv3d::detail::read_flight(input);
        }
        int ordinal = 0;
        bool failed_results = false;
        const auto render_frame = [&] {
            if (retained_session) { return swiv3d::detail::render_session(session,timing); }
            auto presentation = timing;
            if (failed_results) { presentation.overlays = swiv3d::OverlayMode::none; }
            auto result = mission.render(session.globals,presentation);
            if (failed_results) { swiv3d::detail::draw_menu(result,session.globals,mission.resource(108),*session.frontend); }
            return result;
        };
        for (const auto& segment : route) {
            for (unsigned tick = 0; tick < segment.updates; ++tick) {
                if (retained_session) { swiv3d::detail::advance_session(session,segment.keys); }
                else {
                    mission.advance(failed_results ? swiv3d::KeyboardState{} : segment.keys);
                    if (failed_results) { session.frontend->advance_results(1); }
                    else if (mission.state().phase == swiv3d::MissionPhase::failed) {
                        session.frontend->begin_results(mission.result());
                        mission.begin_results();
                        failed_results = true;
                    }
                }
                if (renders.empty()) {
                    frame = render_frame();
                } else {
                    const auto tick = mission.state().tick;
                    const auto& event = renders.at(tick);
                    if (event.count != renders.at(tick - 1).count) {
                        if (measured) { mission.finish_event_loop(); }
                        timing.frames_per_second = event.rate;
                        frame = render_frame();
                    }
                }
                trace_dispatch();
            }
            const auto name = movie ? "step-" + std::to_string(ordinal) : "tick-" + std::to_string(mission.state().tick);
            if (checkpoint_segments.empty() || checkpoint_segments.count(ordinal) != 0) {
                save_checkpoint(frame,output / name);
            }
            ++ordinal;
        }
        if (measured && (!trace || mission.state().tick + 1 != renders.size())) {
            throw std::runtime_error("Incomplete later mission dispatch trace");
        }
        if (handoff) {
            if (mission.state().phase != swiv3d::MissionPhase::completed) { throw std::runtime_error("Ground handoff did not complete naturally"); }
            for (int tick = 0; tick < summary_updates; ++tick) {
                if (retained_session) { swiv3d::detail::advance_session(session,{}); render_frame(); }
                else { mission.advance({}); mission.render(session.globals); }
            }
            if (session.frontend->state().phase != swiv3d::FrontendPhase::mission_results) {
                mission.begin_results(); session.frontend->begin_results(mission.result());
            }
            swiv3d::detail::checkpoint(mission,retained_session ? render_frame() : mission.render(session.globals),output / "summary");
            const int input_ready = results_updates < 0 ? std::max(110,mission.state().finish_counter + 1) : mission.state().finish_counter + results_updates;
            while (mission.state().finish_counter < input_ready) {
                if (retained_session) { swiv3d::detail::advance_session(session,{}); render_frame(); }
                else {
                    mission.advance({}); session.frontend->advance_results(1);
                    mission.render(session.globals,{40,swiv3d::OverlayMode::none});
                }
            }
            const auto save = [&](const char* name,const auto& bytes) {
                std::ofstream file(output / name,std::ios::binary);
                file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
                if (!file) { throw std::runtime_error("Cannot save ground checkpoint handoff"); }
            };
            save("downloaded-profile.bin",mission.result().profile);
            save("saved-configuration.bin",session.frontend->finish_mission(mission.result()));
            save("checkpoint-configuration.bin",session.frontend->configuration());
            for (int count = 0; count < 8 && session.frontend->state().phase == swiv3d::FrontendPhase::briefing; ++count) {
                session.frontend->press(0x01);
            }
            const auto next = session.frontend->state();
            save("next-profile.bin",next.mission_profile); save("next-configuration.bin",session.frontend->configuration());
            if (next.phase == swiv3d::FrontendPhase::main_menu) {
                std::ofstream state(output / "campaign-end.json");
                state << "{\"phase\":\"main-menu\",\"level\":" << next.level << ",\"selection\":" << next.selection << "}\n";
                return 0;
            }
            if (next.phase != swiv3d::FrontendPhase::mission_request) { throw std::runtime_error("Campaign handoff did not reach a mission or ending"); }
            auto next_path = session.game / next.level_file; next_path.replace_extension(".SLV");
            swiv3d::Mission successor(swiv3d::Archive::open(next_path),next,session.frontend->configuration(),&session.globals);
            swiv3d::detail::checkpoint(successor,successor.render(session.globals),output / "next-mission");
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
