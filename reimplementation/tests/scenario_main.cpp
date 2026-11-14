#include "../src/input_script.hpp"
#include "../src/checkpoint.hpp"
#include "enemy_fixture.hpp"

#include <iostream>

int main(int argc, char** argv) {
    try {
        if ((argc != 8 && argc != 10) || std::string(argv[5]) != "--combat" || std::string(argv[6]) != "--fixture" ||
            (argc == 10 && std::string(argv[8]) != "--enemy-gun")) {
            throw std::runtime_error("Usage: swiv3d_scenario GAME FRONTEND MOVIE OUTPUT --combat --fixture LOADOUT");
        }
        const std::filesystem::path game(argv[1]), output(argv[4]);
        auto globals = swiv3d::Archive::open(game / "GLOBAL.BD");
        std::ifstream config(game / "SWIV.CFG", std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(config), {}};
        swiv3d::Frontend frontend(globals, bytes);
        std::ifstream startup(argv[2]);
        swiv3d::detail::replay_frontend(frontend, startup);
        auto request = frontend.state();
        std::ifstream fixture(argv[7]);
        std::string format, extra;
        int slot, ammo, level, health;
        if (!(fixture >> format >> slot >> ammo >> level >> health) || fixture >> extra || format != "swiv3d-loadout-v1" ||
            slot < -1 || slot >= 20 || ammo < 0 || ammo > 500 || level < 0 || level > 19 || health < 0 || health > 992) {
            throw std::runtime_error("Invalid controlled loadout fixture");
        }
        if (slot >= 0) {
            request.mission_profile[slot * 2] = static_cast<std::uint8_t>(ammo);
            request.mission_profile[slot * 2 + 1] = static_cast<std::uint8_t>(ammo >> 8);
        }
        for (int i = 0; i < 4; ++i) {
            request.mission_profile[48 + i] = static_cast<std::uint8_t>(health >> (8 * i));
            request.mission_profile[52 + i] = static_cast<std::uint8_t>(level >> (8 * i));
        }
        auto path = game / request.level_file;
        path.replace_extension(".SLV");
        auto level_archive = swiv3d::Archive::open(path);
        if (argc == 10) { level_archive = scenario::enemy_gun(level_archive, std::stoi(argv[9])); }
        swiv3d::Mission mission(level_archive, request, frontend.configuration(), &globals);
        if (std::filesystem::exists(output)) { throw std::runtime_error("Scenario output must be fresh"); }
        std::filesystem::create_directories(output);
        auto frame = mission.render(globals);
        swiv3d::detail::checkpoint(mission, frame, output / "initial");
        std::ifstream movie(argv[3]);
        int ordinal = 0;
        for (const auto& segment : swiv3d::detail::read_flight(movie)) {
            for (unsigned i = 0; i < segment.updates; ++i) { mission.advance(segment.keys); frame = mission.render(globals); }
            swiv3d::detail::checkpoint(mission, frame, output / ("step-" + std::to_string(ordinal++)));
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
