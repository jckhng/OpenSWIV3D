#include "../src/input_script.hpp"
#include "../src/checkpoint.hpp"
#include "../src/menu_render.hpp"

#include <iostream>

namespace {
template<class Bytes>
void save(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!file) { throw std::runtime_error("Cannot write campaign checkpoint"); }
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 6) { throw std::runtime_error("Expected GAME FRONTEND MOVIE LOADOUT OUTPUT"); }
        const std::filesystem::path game(argv[1]), output(argv[5]);
        if (std::filesystem::exists(output)) { throw std::runtime_error("Campaign output must be fresh"); }
        std::filesystem::create_directories(output);
        auto globals = swiv3d::Archive::open(game / "GLOBAL.BD");
        std::ifstream configuration(game / "SWIV.CFG", std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(configuration), {}};
        swiv3d::Frontend frontend(globals, bytes);
        std::ifstream startup(argv[2]);
        swiv3d::detail::replay_frontend(frontend, startup);
        auto request = frontend.state();
        std::ifstream fixture(argv[4]);
        std::string format;
        int slot, ammo, level, health;
        if (!(fixture >> format >> slot >> ammo >> level >> health) || format != "swiv3d-loadout-v1") { throw std::runtime_error("Invalid loadout"); }
        if (slot >= 0) {
            request.mission_profile.at(slot * 2) = static_cast<std::uint8_t>(ammo);
            request.mission_profile.at(slot * 2 + 1) = static_cast<std::uint8_t>(ammo >> 8);
        }
        for (int i = 0; i < 4; ++i) {
            request.mission_profile[48 + i] = static_cast<std::uint8_t>(health >> (i * 8));
            request.mission_profile[52 + i] = static_cast<std::uint8_t>(level >> (i * 8));
        }
        auto path = game / request.level_file;
        path.replace_extension(".SLV");
        swiv3d::Mission mission(swiv3d::Archive::open(path), request, frontend.configuration(), &globals);
        std::ifstream movie(argv[3]);
        bool results = false;
        for (const auto& segment : swiv3d::detail::read_flight(movie)) {
            for (unsigned i = 0; i < segment.updates; ++i) {
                mission.advance(segment.keys);
                if (results) { frontend.advance_results(1); }
                if (!results && mission.state().phase != swiv3d::MissionPhase::playing) {
                    frontend.begin_results(mission.result());
                    mission.begin_results();
                    results = true;
                    std::vector<std::uint8_t> rows;
                    for (const auto& row : frontend.rows()) {
                        const auto offset = rows.size();
                        rows.resize(offset + 56);
                        std::copy_n(row.text.begin(), std::min(std::size_t{39}, row.text.size()), rows.begin() + offset);
                        const std::array<std::int32_t, 4> fields{row.value, row.colour, row.kind, row.option};
                        for (std::size_t f = 0; f < fields.size(); ++f) {
                            for (int byte = 0; byte < 4; ++byte) { rows[offset + 40 + f * 4 + byte] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(fields[f]) >> (byte * 8)); }
                        }
                    }
                    save(output / "summary-rows.bin", rows);
                    auto frame = mission.render(globals, {40, swiv3d::OverlayMode::none});
                    swiv3d::detail::draw_menu(frame, globals, mission.resource(108), frontend);
                    swiv3d::detail::checkpoint(mission, frame, output / "summary");
                    for (std::size_t y = 0; y < swiv3d::image_height; ++y) {
                        for (std::size_t x = 0; x < swiv3d::image_width; ++x) { frame.pixels[y * swiv3d::image_width + x] = static_cast<std::uint8_t>((x / 8 + y / 5) % 256); }
                    }
                    swiv3d::Frontend presenter_fixture(globals, bytes);
                    std::ifstream presenter_startup(argv[2]);
                    swiv3d::detail::replay_frontend(presenter_fixture, presenter_startup);
                    presenter_fixture.begin_results(mission.result());
                    presenter_fixture.advance_results(4);
                    swiv3d::detail::draw_menu(frame, globals, mission.resource(108), presenter_fixture);
                    save(output / "synthetic-summary.bin", frame.pixels);
                } else { mission.render(globals, {40, results ? swiv3d::OverlayMode::none : swiv3d::OverlayMode::mission}); }
            }
        }
        const auto result = mission.result();
        save(output / "downloaded-profile.bin", result.profile);
        save(output / "saved-configuration.bin", frontend.finish_mission(result));
        save(output / "checkpoint-configuration.bin", frontend.configuration());
        if (frontend.state().phase == swiv3d::FrontendPhase::briefing) { frontend.press(0x01); }
        const auto next = frontend.state();
        save(output / "next-profile.bin", next.mission_profile);
        save(output / "next-configuration.bin", frontend.configuration());
        std::ofstream state(output / "campaign.json");
        state << "{\"level\":" << next.level << ",\"level_file\":\"" << next.level_file << "\",\"sequence_offset\":"
              << next.sequence_offset << ",\"elapsed_updates\":" << result.elapsed_updates << "}\n";
        path = game / next.level_file;
        path.replace_extension(".SLV");
        swiv3d::Mission successor(swiv3d::Archive::open(path), next, frontend.configuration(), &globals);
        swiv3d::detail::checkpoint(successor, successor.render(globals), output / "next-mission");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
