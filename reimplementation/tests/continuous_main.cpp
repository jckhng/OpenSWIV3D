#include "../src/input_script.hpp"
#include "../src/checkpoint.hpp"
#include "scenario.hpp"

#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
    try {
        if (argc != 5 && ((argc != 8 && argc != 10) || std::string(argv[5]) != "--campaign")) { throw std::runtime_error("Expected GAME FRONTEND RENDER_EVENTS OUTPUT [--campaign PREFIX LOADOUT [--level LEVEL]]"); }
        auto session = swiv3d::detail::start_mission(argv[1], argv[2]);
        if (argc >= 8) { scenario::loadout(session, argv[7]); scenario::campaign_prefix(session, argv[6]); }
        if (argc == 10) {
            if (std::string(argv[8]) != "--level") { throw std::runtime_error("Invalid later-level fixture"); }
            auto request = session.frontend->state();
            request.level_file = argv[9];
            auto path = session.game / request.level_file;
            path.replace_extension(".SLV");
            session.mission = std::make_unique<swiv3d::Mission>(swiv3d::Archive::open(path), request,
                                                             session.frontend->configuration(), &session.globals);
        }
        const std::filesystem::path output(argv[4]);
        if (std::filesystem::exists(output)) { throw std::runtime_error("Render output must be fresh"); }
        std::filesystem::create_directories(output);
        std::ifstream input(argv[3]);
        std::string record;
        std::getline(input, record);
        if (record != "swiv3d-render-events-v1") { throw std::runtime_error("Invalid rendering events"); }
        int ordinal = 0;
        while (std::getline(input, record)) {
            std::istringstream row(record);
            std::uint32_t tick;
            int rate;
            std::string keys;
            if (!(row >> tick >> rate >> keys) || tick < session.mission->state().tick) { throw std::runtime_error("Invalid render event"); }
            std::istringstream flight("swiv3d-flight-input-v1\n1 " + keys + "\n");
            const auto held = swiv3d::detail::read_flight(flight).front().keys;
            while (session.mission->state().tick < tick) {
                session.mission->advance(held);
                if (session.audio) {
                    const auto update = session.mission->state().tick;
                    session.audio->mix((static_cast<std::uint64_t>(update) * swiv3d::detail::audio_rate / swiv3d::mission_tick_rate) -
                                       (static_cast<std::uint64_t>(update - 1) * swiv3d::detail::audio_rate / swiv3d::mission_tick_rate));
                }
            }
            swiv3d::RenderTiming timing{rate};
            int cheats = 0;
            if (row >> cheats) { timing.cheats_unlocked = cheats != 0; timing.shortcut_keys = held; }
            const auto frame = session.mission->render(session.globals, timing);
            swiv3d::detail::present_audio(session, frame);
            const auto directory = output / ("frame-" + std::to_string(ordinal++));
            swiv3d::detail::checkpoint(*session.mission, frame, directory);
            std::ofstream controls(directory / "camera-controls.bin",std::ios::binary);
            controls.write(reinterpret_cast<const char*>(frame.camera_controls.data()),sizeof(std::int32_t)*frame.camera_controls.size());
            std::ofstream palette(directory / "palette.bin",std::ios::binary);
            palette.write(reinterpret_cast<const char*>(frame.palette.data()),frame.palette.size());
            std::ofstream voices(directory / "voices.json");
            voices << '[';
            const char* voice_separator = "";
            for (const auto& voice : session.audio->voices()) {
                voices << voice_separator << "[" << voice.sound << ',' << voice.actor << ',' << voice.flags << ','
                       << voice.rate << ',' << voice.volume << ',' << voice.pan << ']';
                voice_separator = ",";
            }
            voices << "]\n";
            std::ofstream camera(directory / "camera.bin", std::ios::binary);
            camera.write(reinterpret_cast<const char*>(frame.camera_target.data()), sizeof(float) * 3);
            camera.write(reinterpret_cast<const char*>(frame.camera_position.data()), sizeof(float) * 3);
            camera.write(reinterpret_cast<const char*>(&frame.camera_tilt), sizeof(float));
            std::ofstream transform(directory / "view-transform.bin", std::ios::binary);
            transform.write(reinterpret_cast<const char*>(frame.view_transform.data()), sizeof(std::int32_t) * frame.view_transform.size());
            std::ofstream markers(directory / "target-markers.bin", std::ios::binary);
            markers.write(reinterpret_cast<const char*>(frame.target_markers.data()), sizeof(std::int32_t) * frame.target_markers.size());
            std::ofstream polygons(directory / "polygons.json");
            polygons << '[';
            const char* separator = "";
            for (const auto& polygon : frame.polygons) {
                polygons << separator << '[';
                const char* field_separator = "";
                for (int value : polygon) { polygons << field_separator << value; field_separator = ","; }
                polygons << ']';
                separator = ",";
            }
            polygons << "]\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
