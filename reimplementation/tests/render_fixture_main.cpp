#include "../src/input_script.hpp"
#include "../src/mission_render.hpp"

#include <algorithm>
#include <iostream>
#include <limits>

namespace {
void save_pixels(const std::filesystem::path& path, const swiv3d::MissionFrame& frame) {
    std::ofstream output(path,std::ios::binary);
    output.write(reinterpret_cast<const char*>(frame.pixels.data()),frame.pixels.size());
    if (!output) { throw std::runtime_error("Cannot write beam fixture pixels"); }
}

void beam_cases(swiv3d::MissionState state, const swiv3d::FrontendState& request,
                std::vector<std::uint8_t> bytes, const std::map<std::uint32_t,swiv3d::Resource>& directory,
                swiv3d::Mission& mission, const swiv3d::Archive& globals, int selected,
                const std::filesystem::path& fixture, const std::filesystem::path& output) {
    auto actor = std::find_if(state.entities.begin(),state.entities.end(),[&](const auto& value) { return value.slot == selected; });
    auto parent = std::find_if(state.entities.begin(),state.entities.end(),[&](const auto& value) {
        return value.slot != selected && value.slot != state.player;
    });
    if (parent == state.entities.end()) { throw std::runtime_error("No beam parent fixture actor"); }
    const auto model = actor->words[19];
    for (auto& entity : state.entities) { entity.words[19] = 0; }
    actor->words[19] = model;
    actor->words[23] = parent->slot;
    std::fill_n(bytes.begin() + state.models.at(model) + 4,4,std::uint8_t{0});
    std::ifstream input(fixture);
    std::array<int,6> coordinates{};
    int ordinal = 0;
    while (input >> coordinates[0] >> coordinates[1] >> coordinates[2] >> coordinates[3] >> coordinates[4] >> coordinates[5]) {
        for (int axis = 0; axis < 3; ++axis) {
            actor->words[7 + axis] = coordinates[axis] * 256;
            parent->words[7 + axis] = coordinates[axis + 3] * 256;
        }
        const auto render = [&] {
            return swiv3d::detail::render_mission(state,request,bytes,directory,
                [&](int x,int z) { return mission.height(x,z); },&globals);
        };
        actor->words[1] &= ~0x400;
        const auto base = render();
        actor->words[1] |= 0x400;
        const auto frame = render();
        save_pixels(output / ("beam-" + std::to_string(ordinal) + "-base.bin"),base);
        save_pixels(output / ("beam-" + std::to_string(ordinal) + ".bin"),frame);
        std::ofstream metadata(output / ("beam-" + std::to_string(ordinal) + ".json"));
        metadata << "{\"actor\":" << actor->slot << ",\"parent\":" << parent->slot << ",\"lines\":[";
        const char* separator = "";
        for (const auto& polygon : frame.polygons) {
            if (polygon[16] != 42) { continue; }
            metadata << separator << '[' << polygon[0] << ',' << polygon[10] << ',' << polygon[11] << ','
                     << polygon[13] << ',' << polygon[14] << ']';
            separator = ",";
        }
        metadata << "]}\n";
        if (!metadata) { throw std::runtime_error("Cannot write beam fixture metadata"); }
        ++ordinal;
    }
    if (!input.eof() || ordinal == 0) { throw std::runtime_error("Invalid beam fixture coordinates"); }
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 4 && (argc != 6 || std::string(argv[4]) != "--beam-cases")) {
            throw std::runtime_error("Expected GAME FRONTEND OUTPUT [--beam-cases FILE]");
        }
        auto session = swiv3d::detail::start_mission(argv[1], argv[2]);
        auto request = session.frontend->state();
        request.landscape_clock = {};
        auto path = session.game / request.level_file;
        path.replace_extension(".SLV");
        auto level = swiv3d::Archive::open(path);
        swiv3d::Mission mission(level, request, session.frontend->configuration(), &session.globals);
        auto state = mission.state();
        const auto player = std::find_if(state.entities.begin(), state.entities.end(), [&](const auto& actor) { return actor.slot == state.player; });
        auto selected = state.entities.end();
        auto nearest = std::numeric_limits<std::int64_t>::max();
        for (auto actor = state.entities.begin(); actor != state.entities.end(); ++actor) {
            if (actor->slot == state.player || actor->words[19] <= 0 || actor->words[19] >= 1000) { continue; }
            std::int64_t distance = 0;
            for (int axis : {7,9}) {
                const auto delta = std::int64_t(actor->words[axis]) - player->words[axis];
                distance += delta * delta;
            }
            if (distance < nearest) { nearest = distance; selected = actor; }
        }
        if (selected == state.entities.end()) { throw std::runtime_error("No tether fixture actor"); }
        selected->words[1] |= 0x400;
        selected->words[23] = state.player;
        std::vector<std::uint8_t> bytes;
        std::map<std::uint32_t, swiv3d::Resource> directory;
        for (const auto& entry : level.resources()) {
            directory.emplace(entry.id, entry);
            bytes.resize(std::max(bytes.size(), std::size_t(entry.offset) + entry.size));
            const auto member = level.load(entry.id);
            std::copy(member.begin(), member.end(), bytes.begin() + entry.offset);
        }
        if (argc == 6) {
            const std::filesystem::path output(argv[3]);
            if (std::filesystem::exists(output)) { throw std::runtime_error("Beam fixture output must be fresh"); }
            std::filesystem::create_directories(output);
            const auto selected_slot = selected->slot;
            beam_cases(std::move(state),request,std::move(bytes),directory,mission,session.globals,
                       selected_slot,argv[5],output);
            return 0;
        }
        const auto frame = swiv3d::detail::render_mission(state, request, bytes, directory,
            [&](int x, int z) { return mission.height(x, z); }, &session.globals);
        const std::filesystem::path output(argv[3]);
        if (std::filesystem::exists(output)) { throw std::runtime_error("Render fixture output must be fresh"); }
        std::filesystem::create_directories(output);
        std::ofstream pixels(output / "world-indexed.bin", std::ios::binary);
        pixels.write(reinterpret_cast<const char*>(frame.pixels.data()), frame.pixels.size());
        std::ofstream metadata(output / "fixture.json");
        metadata << "{\"slot\":" << selected->slot << ",\"lines\":[";
        const char* separator = "";
        for (const auto& polygon : frame.polygons) {
            if (polygon[16] != 42) { continue; }
            metadata << separator << '[' << polygon[0] << ',' << polygon[10] << ',' << polygon[11] << ',' << polygon[13] << ',' << polygon[14] << ']';
            separator = ",";
        }
        metadata << "]}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
