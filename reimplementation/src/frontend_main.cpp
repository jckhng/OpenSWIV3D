#include "swiv3d/frontend.hpp"
#include "swiv3d/mission.hpp"
#include "input_script.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace {

std::string quoted(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char character : value) {
        if (character == '"' || character == '\\') {
            out << '\\' << character;
        } else if (character < 32 || character >= 127) {
            out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(character);
        } else {
            out << character;
        }
    }
    out << '"';
    return out.str();
}

void write(const std::filesystem::path& path, const std::uint8_t* data, std::size_t size) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!stream) {
        throw std::runtime_error("Cannot write frontend checkpoint");
    }
}

void checkpoint(swiv3d::Frontend& frontend, const std::filesystem::path& root, std::size_t ordinal) {
    const auto state = frontend.state();
    const auto rows = frontend.rows();
    const auto output = root / ("step-" + std::to_string(ordinal));
    std::filesystem::create_directory(output);
    const auto configuration = frontend.configuration();
    write(output / "config.bin", configuration.data(), configuration.size());
    write(output / "mission-profile.bin", state.mission_profile.data(), state.mission_profile.size());
    std::ofstream json(output / "state.json");
    json << "{\"phase\":" << quoted(swiv3d::phase_name(state.phase)) << ",\"selection\":" << state.selection
         << ",\"width\":" << state.width << ",\"player\":" << state.player << ",\"language\":" << state.language
         << ",\"level\":" << state.level << ",\"water\":" << state.water << ",\"craft\":" << state.craft
         << ",\"landscape_clock\":{\"rotor\":" << state.landscape_clock.rotor << ",\"water\":" << state.landscape_clock.water << '}'
         << ",\"sequence_offset\":" << state.sequence_offset << ",\"movie\":" << quoted(state.movie)
         << ",\"level_file\":" << quoted(state.level_file) << ",\"tracks\":[";
    const char* separator = "";
    for (auto track : state.tracks) {
        json << separator << track;
        separator = ",";
    }
    json << "],\"rows\":[";
    separator = "";
    for (const auto& row : rows) {
        json << separator << "{\"text\":" << quoted(row.text) << ",\"value\":" << row.value
             << ",\"colour\":" << row.colour << ",\"kind\":" << row.kind << ",\"option\":" << row.option << '}';
        separator = ",";
    }
    json << "]}\n";
    if (!json) {
        throw std::runtime_error("Cannot write frontend state");
    }
    std::cout << ordinal << ' ' << swiv3d::phase_name(state.phase) << '\n';
}

void mission_checkpoint(const swiv3d::Mission& mission, const swiv3d::Archive& globals,
                        const swiv3d::LandscapeClock& clock, const std::filesystem::path& output) {
    std::filesystem::create_directory(output);
    const auto state = mission.state();
    std::ofstream json(output / "state.json");
    json << "{\"free_slot\":" << state.free_slot << ",\"player\":" << state.player
         << ",\"objectives\":" << state.objectives << ",\"pickups\":" << state.pickups
         << ",\"secrets\":" << state.secrets << ",\"line_colour\":" << static_cast<unsigned>(state.line_colour);
    const auto array = [&](const char* name, const auto& values) {
        json << ",\"" << name << "\":[";
        const char* separator = "";
        for (auto value : values) {
            json << separator << value;
            separator = ",";
        }
        json << ']';
    };
    array("models", state.models);
    array("textures", state.textures);
    array("filters", state.filters);
    array("grid", state.grid);
    json << ",\"entities\":[";
    const char* separator = "";
    for (const auto& entity : state.entities) {
        json << separator << "{\"slot\":" << entity.slot << ",\"behavior\":"
             << quoted(swiv3d::behavior_name(static_cast<swiv3d::Behavior>(entity.words[26]))) << ",\"words\":[";
        const char* word_separator = "";
        for (auto value : entity.words) {
            json << word_separator << value;
            word_separator = ",";
        }
        json << "]}";
        separator = ",";
    }
    json << "]}\n";
    for (auto id : {100U, 102U, 103U, 105U, 106U, 107U, 108U, 109U, 110U, 11003U, 11004U}) {
        const auto data = mission.resource(id);
        write(output / ("resource-" + std::to_string(id) + ".bin"), data.data(), data.size());
    }
    const auto frame = mission.first_frame(globals);
    std::ofstream metadata(output / "frame.json");
    metadata << "{\"scope\":\"first mission world and HUD pass\","
             << "\"palette\":\"level resource preview; live DAC unqualified\","
             << "\"landscape_clock\":{\"rotor\":" << clock.rotor << ",\"water\":" << clock.water << "},"
             << "\"qualification\":\"use verify_render.py for zero phase or verify_clock.py for retained update events; hardware scheduling and live display cadence unqualified\"}\n";
    write(output / "frame-indexed.bin", frame.pixels.data(), frame.pixels.size());
    write(output / "terrain-indexed.bin", frame.terrain_pixels.data(), frame.terrain_pixels.size());
    write(output / "world-indexed.bin", frame.world_pixels.data(), frame.world_pixels.size());
    write(output / "stats-indexed.bin", frame.stats_pixels.data(), frame.stats_pixels.size());
    std::ofstream hud(output / "hud.json");
    hud << "{\"objective_heading\":" << frame.objective_heading << ",\"objective_targets\":[";
    separator = "";
    for (const auto& target : state.objective_targets) {
        hud << separator << "{\"entity\":" << target.entity << ",\"priority\":" << target.priority << '}';
        separator = ",";
    }
    hud << "]}\n";
    std::ofstream polygons(output / "polygons.json");
    polygons << '[';
    separator = "";
    for (const auto& polygon : frame.polygons) {
        polygons << separator << '[';
        const char* entry_separator = "";
        for (auto value : polygon) {
            polygons << entry_separator << value;
            entry_separator = ",";
        }
        polygons << ']';
        separator = ",";
    }
    polygons << "]\n";
    write(output / "level-palette.bin", frame.palette.data(), frame.palette.size());
    std::ofstream camera(output / "camera.json");
    camera << "{\"position\":[" << frame.camera_position[0] << ',' << frame.camera_position[1] << ',' << frame.camera_position[2]
           << "],\"target\":[" << frame.camera_target[0] << ',' << frame.camera_target[1] << ',' << frame.camera_target[2] << "]}\n";
    std::ofstream preview(output / "frame.ppm", std::ios::binary);
    constexpr std::size_t preview_width = 640;
    constexpr std::size_t preview_height = 480;
    preview << "P6\n" << preview_width << ' ' << preview_height << "\n255\n";
    // Presentation aspect correction leaves the comparison framebuffer unchanged.
    for (std::size_t y = 0; y < preview_height; ++y) {
        for (std::size_t x = 0; x < preview_width; ++x) {
            const auto pixel = frame.pixels[(y * swiv3d::image_height / preview_height) * swiv3d::image_width + x * swiv3d::image_width / preview_width];
            for (std::size_t channel = 0; channel < 3; ++channel) {
                preview.put(static_cast<char>(frame.palette[pixel * 3 + channel] * 4));
            }
        }
    }
}

}

int main(int argc, char** argv) {
    try {
        if (argc != 4) {
            throw std::runtime_error("Usage: swiv3d_frontend GAME_DIRECTORY INPUT_SCRIPT OUTPUT_DIRECTORY");
        }
        const std::filesystem::path game = argv[1];
        std::ifstream config(game / "SWIV.CFG", std::ios::binary);
        if (!config) {
            throw std::runtime_error("Cannot read original SWIV.CFG");
        }
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(config), {}};
        const auto globals = swiv3d::Archive::open(game / "GLOBAL.BD");
        swiv3d::Frontend frontend(globals, bytes);
        std::ifstream script(argv[2]);
        const std::filesystem::path output = argv[3];
        if (std::filesystem::exists(output)) {
            throw std::runtime_error("Output directory must be fresh");
        }
        std::filesystem::create_directories(output);
        std::size_t ordinal = 0;
        swiv3d::detail::replay_frontend(frontend, script, [&] {
            checkpoint(frontend, output, ordinal++);
        });
        if (frontend.state().phase == swiv3d::FrontendPhase::mission_request) {
            auto level_path = game / frontend.state().level_file;
            level_path.replace_extension(".SLV");
            const auto level = swiv3d::Archive::open(level_path);
            swiv3d::Mission mission(level, frontend.state(), frontend.configuration());
            mission_checkpoint(mission, globals, frontend.state().landscape_clock, output / "mission");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
