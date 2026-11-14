#include "../src/input_script.hpp"
#include "../src/checkpoint.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace {
enum class Boundary { reset, render };

struct Event {
    Boundary boundary;
    int rate = 0;
};

struct Cadence {
    unsigned tick, renders;
    int rate;
    unsigned capture;
    std::vector<Event> events;
};
}

int main(int argc, char** argv) {
    try {
        if (argc != 6) { throw std::runtime_error("Expected GAME FRONTEND MOVIE CADENCE OUTPUT"); }
        std::ifstream events(argv[4]);
        std::string header;
        std::getline(events,header);
        const bool measured = header == "swiv3d-campaign-audit-v2";
        if (!measured && header != "swiv3d-campaign-audit-v1") { throw std::runtime_error("Invalid campaign audit cadence"); }
        std::vector<Cadence> cadence;
        std::string line;
        while (std::getline(events,line)) {
            std::istringstream row(line);
            Cadence entry{};
            if (!(row >> entry.tick >> entry.renders >> entry.rate >> entry.capture) ||
                entry.tick != cadence.size() || entry.capture > 1) {
                throw std::runtime_error("Unsupported campaign audit cadence");
            }
            unsigned renders = 0;
            std::string kind;
            while (row >> kind) {
                if (measured && entry.tick != 0 && kind == "reset") {
                    entry.events.push_back({Boundary::reset});
                } else if (measured && entry.tick != 0 && kind == "render") {
                    int rate;
                    if (!(row >> rate) || rate <= 0) { throw std::runtime_error("Invalid audit render rate"); }
                    entry.events.push_back({Boundary::render,rate});
                    ++renders;
                } else {
                    throw std::runtime_error("Invalid campaign audit event");
                }
            }
            if (!cadence.empty()) {
                const auto previous = cadence.back().renders;
                if (!measured && entry.renders == previous + 1) {
                    entry.events.push_back({Boundary::render,entry.rate});
                    ++renders;
                }
                if (entry.renders != previous + renders) { throw std::runtime_error("Incomplete audit render cadence"); }
            }
            cadence.push_back(std::move(entry));
        }
        if (!events.eof() || cadence.empty()) { throw std::runtime_error("Incomplete campaign audit cadence"); }
        const std::filesystem::path output(argv[5]);
        if (std::filesystem::exists(output)) { throw std::runtime_error("Campaign audit output must be fresh"); }
        auto session = swiv3d::detail::start_mission(argv[1],argv[2]);
        const auto initial_level = session.frontend->state().level;
        if (session.frontend->state().cheat_mode) { throw std::runtime_error("Normal-inventory audit entered Test Levels"); }
        std::filesystem::create_directories(output);
        swiv3d::RenderTiming timing;
        if (measured) { timing.counter_reset = swiv3d::FrameCounterReset::event_loop_end; }
        const auto render = [&](int rate) {
            timing.frames_per_second = rate;
            return swiv3d::detail::render_session(session,timing);
        };
        auto frame = render(cadence.front().rate);
        std::ofstream trace(output / "updates.csv");
        trace << "tick,health,primary,selected,requested,shots,kills,collected,objectives,gun_pitch,target,x,y,z,ammunition,game_counter,health_bar\n";
        const auto checkpoint = [&](unsigned tick) {
            const auto state = session.mission->state();
            const auto player = std::find_if(state.entities.begin(),state.entities.end(),
                [&](const auto& actor) { return actor.slot == state.player; });
            trace << tick << ',' << player->words[2] << ',' << state.weapons.primary_level << ',' << state.weapons.selected
                  << ',' << state.weapons.requested << ',' << state.shots << ',' << state.kills << ',' << state.weapons.collected
                  << ',' << state.objectives_completed << ',' << state.gun_pitch << ',' << player->words[22];
            for (int axis = 0; axis < 3; ++axis) { trace << ',' << player->words[7+axis]; }
            trace << ',';
            const char* separator = "";
            for (int ammo : state.weapons.ammunition) { trace << separator << ammo; separator = "+"; }
            trace << ',' << state.game_counter << ',' << state.health_bar << '\n';
            if (cadence.at(tick).capture == 0) { return; }
            const auto folder = output / ("tick-" + std::to_string(tick));
            swiv3d::detail::checkpoint(*session.mission,frame,folder);
            std::ofstream palette(folder / "palette.bin",std::ios::binary);
            palette.write(reinterpret_cast<const char*>(frame.palette.data()),frame.palette.size());
            std::ofstream transform(folder / "view-transform.bin",std::ios::binary);
            transform.write(reinterpret_cast<const char*>(frame.view_transform.data()),sizeof(frame.view_transform));
            std::ofstream terrain(folder / "terrain-indexed.bin",std::ios::binary);
            terrain.write(reinterpret_cast<const char*>(frame.terrain_pixels.data()),frame.terrain_pixels.size());
            std::ofstream polygons(folder / "polygons.bin",std::ios::binary);
            polygons.write(reinterpret_cast<const char*>(frame.polygons.data()),frame.polygons.size()*sizeof(frame.polygons.front()));
            if (!palette || !transform || !terrain || !polygons) { throw std::runtime_error("Cannot write audit presentation"); }
        };
        checkpoint(0);
        std::ifstream movie(argv[3]);
        unsigned tick = 0;
        for (const auto& segment : swiv3d::detail::read_flight(movie)) {
            for (unsigned update = 0; update < segment.updates; ++update) {
                swiv3d::detail::advance_session(session,segment.keys);
                ++tick;
                if (session.frontend->state().level != initial_level || session.mission->state().tick != tick) {
                    throw std::runtime_error("Audit left its retained mission");
                }
                for (const auto& event : cadence.at(tick).events) {
                    if (event.boundary == Boundary::reset) { session.mission->finish_event_loop(); }
                    else { frame = render(event.rate); }
                }
                checkpoint(tick);
            }
        }
        if (tick + 1 != cadence.size() || !trace) { throw std::runtime_error("Audit did not consume its complete cadence"); }
        std::cout << "Normal-inventory first mission audited through " << tick << " updates\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
