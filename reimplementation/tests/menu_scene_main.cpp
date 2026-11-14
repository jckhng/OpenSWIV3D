#include "../src/checkpoint.hpp"
#include "../src/menu_render.hpp"
#include "../src/profile_file.hpp"
#include "../src/input_script.hpp"

#include <fstream>
#include <iostream>

namespace {

void save_frame(const swiv3d::Mission& mission, const swiv3d::MissionFrame& frame, const std::filesystem::path& path) {
    swiv3d::detail::checkpoint(mission,frame,path);
    std::ofstream matrix(path / "view-transform.bin",std::ios::binary);
    matrix.write(reinterpret_cast<const char*>(frame.view_transform.data()),sizeof(frame.view_transform));
    std::ofstream palette(path / "palette.bin",std::ios::binary);
    palette.write(reinterpret_cast<const char*>(frame.palette.data()),frame.palette.size());
    std::ofstream geometry(path / "polygons.bin",std::ios::binary);
    geometry.write(reinterpret_cast<const char*>(frame.polygons.data()),frame.polygons.size()*sizeof(frame.polygons.front()));
    if (!matrix || !palette || !geometry) { throw std::runtime_error("Cannot write menu render state"); }
}

}

int main(int argc, char** argv) {
    try {
        if (argc != 4 && argc != 5) { throw std::runtime_error("Expected GAME CADENCE OUTPUT [--route|--mission=FRONTEND]"); }
        const std::filesystem::path game = argv[1], output = argv[3];
        if (std::filesystem::exists(output)) { throw std::runtime_error("Output must be fresh"); }
        const std::string option = argc == 5 ? argv[4] : "";
        const bool mission_route = option.rfind("--mission=",0) == 0;
        auto session = mission_route ? swiv3d::detail::start_mission(game,option.substr(10)) :
            swiv3d::detail::start_frontend(game);
        if (!mission_route) { swiv3d::detail::complete_cutscene(session,swiv3d::detail::CutsceneEnd::skipped); }
        if (mission_route) { session.profile = output / "pilot.CFG"; }
        const auto mission = [&]() -> swiv3d::Mission& { return *session.mission; };
        std::filesystem::create_directories(output);
        swiv3d::MissionFrame entry;
        entry.pixels.resize(swiv3d::image_width * swiv3d::image_height);
        swiv3d::detail::checkpoint(mission(),entry,output / "entry");
        std::ifstream cadence(argv[2]);
        if (argc == 5) {
            if (option != "--route" && !mission_route) { throw std::runtime_error("Unknown menu scene option"); }
            std::string command;
            swiv3d::MissionFrame last_frame;
            while (cadence >> command) {
                if (command == "advance") {
                    unsigned tick;
                    cadence >> tick;
                    while (session.menu_updates < tick) { swiv3d::detail::advance_session(session,{}); }
                    if (session.menu_updates != tick) { throw std::runtime_error("Menu route moved backwards"); }
                } else if (mission_route && command == "game-updates") {
                    unsigned tick;
                    cadence >> tick;
                    while (mission().state().tick < tick) { swiv3d::detail::advance_session(session,{}); }
                    if (mission().state().tick != tick) { throw std::runtime_error("Mission modal route moved backwards"); }
                    mission().finish_event_loop();
                } else if (mission_route && command == "game-render") {
                    int rate;
                    cadence >> rate;
                    swiv3d::RenderTiming timing{rate};
                    timing.counter_reset = swiv3d::FrameCounterReset::event_loop_end;
                    session.render_rate = rate;
                    last_frame = mission().render(session.globals,timing);
                } else if (command == "press" || command == "press-shift") {
                    unsigned scan;
                    cadence >> scan;
                    swiv3d::detail::frontend_key(session,scan,command == "press-shift" ? swiv3d::KeyShift::pressed : swiv3d::KeyShift::released);
                } else if (command == "skip-movie") { swiv3d::detail::complete_cutscene(session,swiv3d::detail::CutsceneEnd::skipped); }
                else if (command == "unhold") { mission().end_menu(); }
                else if (command == "render") {
                    unsigned index, rate, capture;
                    cadence >> index >> rate >> capture;
                    const auto state = session.frontend->state();
                    session.render_rate = rate;
                    const bool grab = state.phase == swiv3d::FrontendPhase::mission_grab || state.phase == swiv3d::FrontendPhase::grab_name;
                    const auto overlays = grab && state.grab_statistics ? swiv3d::OverlayMode::statistics : swiv3d::OverlayMode::none;
                    last_frame = mission().render(session.globals,{static_cast<int>(rate),overlays,state.screen_size,state.detail,state.debug_controls,state.debug_counter});
                    if (capture != 0) { save_frame(mission(),last_frame,output / ("frame-" + std::to_string(index))); }
                } else if (command == "panel") {
                    unsigned index, capture;
                    cadence >> index >> capture;
                    if (last_frame.pixels.empty()) { throw std::runtime_error("Menu panel lacks a landscape frame"); }
                    auto frame = last_frame;
                    swiv3d::detail::draw_menu(frame,session.globals,mission().resource(108),*session.frontend);
                    swiv3d::detail::apply_gamma(frame,session.frontend->state().gamma);
                    if (capture != 0) {
                        const auto path = output / ("frame-" + std::to_string(index));
                        save_frame(mission(),frame,path);
                        const auto config = session.frontend->configuration();
                        std::ofstream configuration(path / "configuration.bin",std::ios::binary);
                        configuration.write(reinterpret_cast<const char*>(config.data()),config.size());
                        const auto state = session.frontend->state();
                        std::ofstream menu_state(path / "menu-state.txt");
                        menu_state << swiv3d::phase_name(state.phase) << ' ' << state.selection << ' ' << state.width << ' '
                                   << state.language << ' ' << state.gamma << ' ' << state.music_type << ' '
                                   << state.screen_size << ' ' << state.detail;
                        for (int volume : state.volumes) { menu_state << ' ' << volume; }
                        menu_state << '\n';
                        std::ofstream menu(path / "menu.bin",std::ios::binary);
                        for (const auto& row : session.frontend->rows()) {
                            std::array<char,40> text{};
                            if (row.text.size() >= text.size()) { throw std::runtime_error("Menu row exceeds captured storage"); }
                            std::copy(row.text.begin(),row.text.end(),text.begin());
                            const std::array<int,4> fields{row.value,row.colour,row.kind,row.option};
                            menu.write(text.data(),text.size());
                            menu.write(reinterpret_cast<const char*>(fields.data()),sizeof(fields));
                        }
                        if (!configuration || !menu_state || !menu) { throw std::runtime_error("Cannot write frontend checkpoint"); }
                    }
                } else { throw std::runtime_error("Unknown menu scene command"); }
                if (!cadence) { throw std::runtime_error("Truncated menu scene command"); }
            }
            const auto config = session.frontend->configuration();
            std::ofstream file(output / "configuration.bin",std::ios::binary);
            file.write(reinterpret_cast<const char*>(config.data()),config.size());
            std::cout << "Menu route updates " << mission().state().tick << '\n';
            return 0;
        }
        unsigned index, tick, rate, capture;
        while (cadence >> index >> tick >> rate >> capture) {
            while (mission().state().tick < tick) { swiv3d::detail::advance_session(session,{}); }
            if (mission().state().tick != tick) { throw std::runtime_error("Menu cadence moved backwards"); }
            auto frame = swiv3d::detail::render_session(session,{static_cast<int>(rate)});
            if (capture != 0) { save_frame(mission(),frame,output / ("frame-" + std::to_string(index))); }
        }
        std::cout << "Menu scene updates " << mission().state().tick << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
