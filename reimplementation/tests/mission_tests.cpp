#include "swiv3d/mission.hpp"
#include "../src/mission_render.hpp"
#include "../src/ground_effects.hpp"
#include "level_fixture.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void put(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8));
    }
}

enum class TestModel { point, double_sided_triangle, animated_triangle };
enum class TestActors { mixed, scenery, demo };

swiv3d::Archive level(std::uint32_t solid_command = 2000, std::uint32_t player_x = 64, std::uint32_t solid_x = 128,
                      TestModel shape = TestModel::point, TestActors actors = TestActors::mixed) {
    std::map<std::uint32_t, std::vector<std::uint8_t>> members;
    for (auto id : {100, 102, 103}) {
        members[id].resize(swiv3d::map_side * swiv3d::map_side, id == 102 ? 200 : 0);
    }
    for (std::size_t z = 0; z < swiv3d::map_side; ++z) {
        for (std::size_t x = 0; x < swiv3d::map_side; ++x) {
            members[100][z * swiv3d::map_side + x] = static_cast<std::uint8_t>((x % 16) * 8 + (z % 8) * 2);
        }
    }
    for (auto id : {106, 107, 108, 109, 110, 11004}) {
        members[id].resize(65536);
    }
    members[500].resize(12288);
    if (actors == TestActors::demo) {
        for (auto id : {504, 505, 506}) { members[id] = {}; }
        constexpr std::size_t node_bytes = 9 * sizeof(std::uint32_t);
        auto& nodes = members[107];
        nodes.resize(2 * node_bytes);
        put(nodes, 4 * sizeof(std::uint32_t), 512);
        put(nodes, 5 * sizeof(std::uint32_t), 512);
        put(nodes, 7 * sizeof(std::uint32_t), 1);
        put(nodes, node_bytes + 6 * sizeof(std::uint32_t), UINT32_MAX);
    }
    members[11003].resize(swiv3d::palette_bytes + 8192, 31);
    auto& texture = members[12000];
    texture.resize(0x8100 + 131072, 5);
    for (std::size_t i = 0; i < 32768; ++i) {
        texture[256 + i] = static_cast<std::uint8_t>(i);
    }
    if (shape != TestModel::point) {
        members[12001].resize(0x8100 + 65536, 7);
        for (std::size_t i = 0; i < 32768; ++i) {
            members[12001][256 + i] = static_cast<std::uint8_t>(i);
        }
    }
    for (auto id : {10800, 10990, 10991, 10980}) {
        auto& model = members[id];
        model.resize(48);
        put(model, 0, 1);
        put(model, 16, static_cast<std::uint32_t>(-12));
        put(model, 24, 32);
        put(model, 28, 32);
        if (shape != TestModel::point) {
            constexpr std::size_t face = 80;
            constexpr std::size_t normal = face + 42;
            model.resize(normal + 12);
            put(model, 0, 3);
            put(model, 4, 1);
            put(model, 8, 1);
            put(model, 20, 30);
            put(model, 24, static_cast<std::uint32_t>(face));
            put(model, 28, static_cast<std::uint32_t>(normal));
            put(model, 32, static_cast<std::uint32_t>(-32));
            put(model, 48, 32);
            put(model, 68, 40);
            put(model, face + 4, 1);
            put(model, face + 8, 2);
            put(model, face + 24, 16);
            model[face + 40] = 2;
            model[face + 41] = shape == TestModel::animated_triangle ? 2 : 0;
            put(model, normal + 4, 256);
        }
    }
    auto& commands = members[105];
    commands.resize(5 * 64);
    const std::array<std::uint32_t, 4> types{actors == TestActors::demo ? 1001U : 1000U, solid_command, actors == TestActors::mixed ? 7000U : 2000U, actors == TestActors::mixed ? 5000U : 2000U};
    for (std::size_t i = 0; i < types.size(); ++i) {
        const auto offset = i * 64;
        put(commands, offset, types[i]);
        put(commands, offset + 4, i == 0 ? player_x : i == 1 ? solid_x : 64 + static_cast<std::uint32_t>(i) * 64);
        put(commands, offset + 8, 128);
        put(commands, offset + 16, 800);
        put(commands, offset + 20, 800);
        put(commands, offset + 24, 200);
        put(commands, offset + 28, i == 1 ? 5002 : 0);
    }
    put(commands, 4 * 64, UINT32_MAX);
    std::vector<swiv3d::Resource> directory;
    std::vector<std::uint8_t> bytes;
    for (const auto& entry : members) {
        directory.push_back({static_cast<std::uint32_t>(bytes.size()), static_cast<std::uint32_t>(entry.second.size()), entry.first});
        bytes.insert(bytes.end(), entry.second.begin(), entry.second.end());
    }
    directory.push_back({static_cast<std::uint32_t>(directory.size() + 1), 0, 0});
    for (const auto& entry : directory) {
        const auto offset = bytes.size();
        bytes.resize(offset + 12);
        put(bytes, offset, entry.offset);
        put(bytes, offset + 4, entry.size);
        put(bytes, offset + 8, entry.id);
    }
    return swiv3d::Archive(std::move(bytes));
}

void test_demo_initialization() {
    swiv3d::FrontendState request;
    request.phase = swiv3d::FrontendPhase::mission_request;
    request.level_file = "DEMO.slv";
    request.mission_profile[48] = 144; request.mission_profile[49] = 1;
    swiv3d::Mission mission(level(2000,64,128,TestModel::point,TestActors::demo),request,{});
    const auto state = mission.state();
    require(state.entities.size() == 4, "Demo created gameplay weapon assemblies");
    const auto& player = state.entities.front();
    require(player.words[1] == 0x40 && player.words[2] == 400 && player.words[19] == 990,
            "Demo lost its dummy player setup");
    require(std::string(swiv3d::behavior_name(static_cast<swiv3d::Behavior>(player.words[26]))) == "OBJECT_Heli1_demo",
            "Demo used the controlled player callback");
    require(state.flight.control == swiv3d::PlayerControl::disabled, "Demo accepted gameplay controls");
}

void test_frame_counter_boundary() {
    const auto archive = level(2000,64,128,TestModel::point,TestActors::demo);
    swiv3d::FrontendState request;
    request.phase = swiv3d::FrontendPhase::mission_request;
    request.level_file = "DEMO.slv";
    constexpr unsigned carried_counter = 49;
    request.landscape_clock.water = 1337;
    request.game_counter = carried_counter;
    request.mission_profile[48] = 144; request.mission_profile[49] = 1;
    swiv3d::Mission mission(archive,request,{});
    swiv3d::RenderTiming timing;
    timing.overlays = swiv3d::OverlayMode::none;
    timing.counter_reset = swiv3d::FrameCounterReset::event_loop_end;

    mission.finish_event_loop();
    mission.render(archive,timing);
    require(mission.state().game_counter == carried_counter, "Render discarded the inherited counter before dispatch");
    mission.advance({});
    mission.advance({});
    require(mission.state().game_counter == carried_counter + 2 && mission.state().health_bar == 1,
            "The dispatch batch lost the shared counter or health bar phase");
    mission.finish_event_loop();
    require(mission.state().game_counter == 0, "Event-loop return did not clear the counter");
    mission.advance({});
    mission.finish_event_loop();
    require(mission.state().game_counter == 1, "The same render caused a second reset");

    mission.render(archive,timing);
    mission.reload_background();
    mission.finish_event_loop();
    require(mission.state().game_counter == 1, "Scene reload discarded the counter or inherited a pending reset");
    timing.counter_reset = swiv3d::FrameCounterReset::render_end;
    mission.render(archive,timing);
    require(mission.state().game_counter == 0, "Render-only playback lost its reset");
}

void test_initialization_and_render() {
    auto archive = level(2000, 64, 128, TestModel::double_sided_triangle);
    swiv3d::FrontendState request;
    request.phase = swiv3d::FrontendPhase::mission_request;
    request.level_file = "TERRAN00.slv";
    request.player = 0;
    request.mission_profile[48] = 240;
    request.mission_profile[49] = 1;
    std::vector<std::uint8_t> config(128);
    put(config, 0, 20);
    put(config, 8, 2);
    put(config, 12, 1);
    swiv3d::Mission mission(archive, request, config);
    const auto state = mission.state();
    require(state.entities.size() == 6 && state.free_slot == 2994, "Object allocation/spawn order changed");
    require(state.entities[0].slot == 3000 && state.entities[1].slot == 2999, "Player consumed a pooled object");
    require(state.entities[0].words[2] == 496 && state.entities[0].words[19] == 990, "Retained mission profile was lost");
    require(state.objectives == 1 && state.pickups == 1 && state.secrets == 0, "Setup counters changed");
    require(state.entities[1].words[1] == 0x2001 && state.entities[5].words[23] == state.entities[4].slot,
            "Objective flag or canonical parent alias was lost");
    require(state.models[0] == state.models[800], "Default model aliases were not retained");
    require(mission.resource(100) == archive.load(100), "Initialization changed the terrain map");
    require(mission.height(64 * 256, 128 * 256) == 12 * 1024, "Height map coordinate scale changed");
    require(mission.height(96 * 256, 128 * 256) == 16 * 1024, "Height interpolation changed");
    require(mission.height(-64 * 256, 0) == 120 * 1024, "Negative map coordinate wrapping changed");
    const auto frame = mission.first_frame();
    require(std::any_of(frame.polygons.begin(), frame.polygons.end(), [](const auto& polygon) { return polygon[16] == 2; }),
            "Double-sided textured polygon did not reach the renderer");
    require(std::count(frame.pixels.begin(), frame.pixels.end(), 7) > 0,
            "Double-sided textured polygon did not draw its texture");
    require(frame.pixels.size() == swiv3d::image_width * swiv3d::image_height, "Mission frame size changed");
    require(std::count(frame.pixels.begin(), frame.pixels.end(), 5) > 1000, "Terrain did not sample the original texture bank");
    require(frame.pixels == mission.first_frame().pixels, "First-frame rendering mutated initialization state");
    bool rejected = false;
    try {
        const auto unsupported = level(9999);
        swiv3d::Mission invalid(unsupported, request, config);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "Unknown setup command silently omitted an entity");
}

void test_flight_grid_crossing() {
    // Occupy both sides of a cell boundary so the original execution plan
    // encounters a helicopter again after it relinks into the later cell.
    const auto archive = level(2000, 511, 600);
    swiv3d::FrontendState request;
    request.phase = swiv3d::FrontendPhase::mission_request;
    request.level_file = "TERRAN00.slv";
    request.player = 0;
    request.mission_profile[48] = 240;
    request.mission_profile[49] = 1;
    std::vector<std::uint8_t> config(128);
    put(config, 0, 20);
    put(config, 8, 2);
    put(config, 12, 1);
    const std::array<std::uint32_t, 11> bindings{0xcb, 0xcd, 0xc8, 0xd0, 0x33, 0x34, 0x39, 0x32, 0xc9, 0xd1, 0x1d};
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        put(config, 16 + i * 4, bindings[i]);
    }
    swiv3d::Mission mission(archive, request, config);
    swiv3d::KeyboardState keys{};
    keys[0x34] = true;
    int previous_age = 0;
    bool repeated = false;
    for (int i = 0; i < 20; ++i) {
        mission.advance_flight(keys);
        const auto state = mission.state();
        const auto& player = state.entities.front();
        repeated |= player.words[6] == previous_age + 2;
        previous_age = player.words[6];
        std::size_t occurrences = 0;
        for (auto head : state.grid) {
            for (auto slot = head; slot != swiv3d::no_entity;) {
                const auto entity = std::find_if(state.entities.begin(), state.entities.end(), [&](const auto& item) { return item.slot == slot; });
                require(entity != state.entities.end(), "Grid link references an unallocated entity");
                occurrences += slot == state.player;
                slot = entity->words[0];
            }
        }
        require(occurrences == 1, "Flight relinking lost or duplicated the player");
        require(state.flight.control_ticks == state.tick && state.landscape_clock.rotor == state.tick * 10,
                "A repeated body update also repeated controls or the animation clock");
    }
    require(repeated, "Flight body no longer follows occupied-cell execution order");
}

void test_restart_resets_owned_world() {
    auto archive = level(2000, 64, 128, TestModel::point, TestActors::scenery);
    swiv3d::FrontendState request;
    request.phase = swiv3d::FrontendPhase::mission_request;
    request.level_file = "TERRAN00.slv";
    request.player = 0;
    std::vector<std::uint8_t> config(128);
    put(config, 0, 20); put(config, 8, 2); put(config, 12, 1);
    swiv3d::Mission mission(archive, request, config);
    const auto initial = mission.state();
    for (int i = 0; i < 36; ++i) { mission.advance({}); }
    require(mission.state().phase == swiv3d::MissionPhase::failed, "Zero-health mission failed to reach results");
    const auto clock = mission.state().landscape_clock;
    mission.restart();
    const auto restarted = mission.state();
    require(restarted.phase == swiv3d::MissionPhase::playing && restarted.finish_counter == -1 && restarted.tick == 0,
            "Restart retained the previous outcome or mission timer");
    require(restarted.grid == initial.grid && restarted.entities.size() == initial.entities.size() &&
            restarted.free_slot == initial.free_slot && restarted.weapons.collected == 0 && restarted.objectives_completed == 0,
            "Restart retained transient entities, pool links or collection state");
    require(mission.resource(100) == archive.load(100) && restarted.landscape_clock.water == clock.water,
            "Restart lost terrain or carried animation clocks");
}

void test_tether_line_rendering() {
    auto archive = level(2000, 64, 128, TestModel::point, TestActors::scenery);
    swiv3d::FrontendState request;
    request.phase = swiv3d::FrontendPhase::mission_request;
    request.player = 0;
    std::vector<std::uint8_t> config(128);
    put(config, 0, 20); put(config, 8, 2); put(config, 12, 1);
    swiv3d::Mission mission(archive, request, config);
    auto state = mission.state();
    state.line_colour = 231;
    state.entities[1].words[1] |= 0x400;
    state.entities[1].words[23] = state.player;
    std::vector<std::uint8_t> bytes;
    std::map<std::uint32_t, swiv3d::Resource> directory;
    for (const auto& entry : archive.resources()) {
        directory.emplace(entry.id, entry);
        bytes.resize(std::max(bytes.size(), std::size_t(entry.offset) + entry.size));
        const auto member = archive.load(entry.id);
        std::copy(member.begin(), member.end(), bytes.begin() + entry.offset);
    }
    const auto frame = swiv3d::detail::render_mission(state, request, bytes, directory,
        [&](int x, int z) { return mission.height(x, z); });
    require(std::any_of(frame.polygons.begin(), frame.polygons.end(), [](const auto& polygon) { return polygon[16] == 42; }),
            "Tether flag did not submit its sorted line primitive");
    require(std::count(frame.pixels.begin(), frame.pixels.end(), 231) != 0, "Sorted tether line did not render");
}

void test_beam_texture_clock() {
    const auto archive = level(2000,64,128,TestModel::animated_triangle,TestActors::scenery);
    swiv3d::FrontendState request;
    request.phase = swiv3d::FrontendPhase::mission_request;
    request.player = 0;
    std::vector<std::uint8_t> config(128);
    put(config,0,20); put(config,8,2); put(config,12,1);
    swiv3d::Mission mission(archive,request,config);
    std::vector<std::uint8_t> bytes;
    std::map<std::uint32_t,swiv3d::Resource> directory;
    for (const auto& entry : archive.resources()) {
        directory.emplace(entry.id,entry);
        bytes.resize(std::max(bytes.size(),std::size_t(entry.offset) + entry.size));
        const auto member = archive.load(entry.id);
        std::copy(member.begin(),member.end(),bytes.begin() + entry.offset);
    }
    const auto check = [&](int flags, unsigned clock, int expected) {
        auto state = mission.state();
        state.landscape_clock.water = clock;
        for (auto& actor : state.entities) { actor.words[1] = (actor.words[1] & ~0x50000) | flags; }
        const auto frame = swiv3d::detail::render_mission(state,request,bytes,directory,
            [&](int x,int z) { return mission.height(x,z); });
        bool found = false;
        for (const auto& polygon : frame.polygons) {
            if (polygon[16] != 2) { continue; }
            found = true;
            require(polygon[7] == expected && polygon[8] == expected && polygon[9] == expected,
                    "Beam face did not retain its clock, hold flag or four-frame UV cycle");
        }
        require(found,"Animated fixture submitted no model faces");
    };
    check(0,7,16);
    check(0x40000,7,48);
    check(0x40000,8,0);
    check(0x50000,7,0);
}

void test_terrain_filter_alias() {
    const auto archive = level();
    swiv3d::FrontendState request;
    request.phase = swiv3d::FrontendPhase::mission_request;
    request.player = 0;
    std::vector<std::uint8_t> config(128);
    put(config,0,20); put(config,8,2); put(config,12,1);
    swiv3d::Mission mission(archive,request,config);
    auto state = mission.state();
    auto& player = state.entities.front().words;
    player[7] = 1726342; player[8] = 93736; player[9] = 778478; player[13] = 386;
    std::vector<std::uint8_t> bytes;
    std::map<std::uint32_t,swiv3d::Resource> directory;
    for (const auto& entry : archive.resources()) {
        directory.emplace(entry.id,entry);
        bytes.resize(std::max(bytes.size(),std::size_t(entry.offset) + entry.size));
        const auto member = archive.load(entry.id);
        std::copy(member.begin(),member.end(),bytes.begin() + entry.offset);
    }
    // A one-pixel initial strip retains shade 99999 and reads beyond the filter table.
    constexpr std::size_t aliased_filter_index = (99999 & ~255) | 5;
    bytes.at(directory.at(12000).offset + 256 + aliased_filter_index) = 231;
    const auto frame = swiv3d::detail::render_mission(state,request,bytes,directory,
        [&](int x,int z) { return mission.height(x,z); });
    require(std::count(frame.terrain_pixels.begin(),frame.terrain_pixels.end(),231) != 0,
            "Terrain filter lookup lost its in-resource texture alias");
}

void test_water_texture_animation() {
    const auto archive = level();
    swiv3d::FrontendState request;
    request.phase = swiv3d::FrontendPhase::mission_request;
    request.player = 0; request.water = 1;
    std::vector<std::uint8_t> config(128);
    put(config,0,20); put(config,8,2); put(config,12,1);
    swiv3d::Mission mission(archive,request,config);
    auto state = mission.state();
    state.entities.front().words[8] = 32000;
    std::vector<std::uint8_t> bytes;
    std::map<std::uint32_t,swiv3d::Resource> directory;
    for (const auto& entry : archive.resources()) {
        directory.emplace(entry.id,entry);
        bytes.resize(std::max(bytes.size(),std::size_t(entry.offset) + entry.size));
        const auto member = archive.load(entry.id);
        std::copy(member.begin(),member.end(),bytes.begin() + entry.offset);
    }
    for (auto id : {100,102}) {
        std::fill_n(bytes.begin() + directory.at(id).offset,directory.at(id).size,0);
    }
    const auto water = directory.at(12000).offset + 0x8100;
    std::fill_n(bytes.begin() + water,4096,237);
    for (int row = 0; row < 64; ++row) {
        for (int column = 0; column < 64; ++column) { bytes.at(water + 4096 + row * 64 + column) = (column * 3 + row * 5) & 127; }
    }
    const auto render = [&](unsigned clock) {
        state.landscape_clock.water = clock;
        return swiv3d::detail::render_mission(state,request,bytes,directory,[](int,int) { return 0; }).terrain_pixels;
    };
    const auto first = render(17);
    require(std::count(first.begin(),first.end(),237) == 0,"Water renderer sampled the unanimated destination tile");
    require(first != render(18),"Water distortion ignored its animation clock");
    require(first == render(529) && render(UINT32_MAX) == render(511),"Water animation lost its wrapped phase");
}

void test_spatial_effect_rendering() {
    const auto archive = level();
    swiv3d::FrontendState request;
    request.phase = swiv3d::FrontendPhase::mission_request;
    request.player = 0;
    std::vector<std::uint8_t> config(128);
    put(config,0,20); put(config,8,2); put(config,12,1);
    swiv3d::Mission mission(archive,request,config);
    auto state = mission.state();
    state.entities.front().words[8] = 32000;
    std::vector<std::uint8_t> bytes;
    std::map<std::uint32_t,swiv3d::Resource> directory;
    for (const auto& entry : archive.resources()) {
        directory.emplace(entry.id,entry);
        bytes.resize(std::max(bytes.size(),std::size_t(entry.offset) + entry.size));
        const auto member = archive.load(entry.id);
        std::copy(member.begin(),member.end(),bytes.begin() + entry.offset);
    }
    std::fill_n(bytes.begin() + directory.at(100).offset,directory.at(100).size,0);
    const auto filters = directory.at(12000).offset + 256;
    for (std::size_t i = 0; i < 32768; ++i) { bytes[filters + i] = static_cast<std::uint8_t>(i >> 8); }
    const auto render = [&] {
        return swiv3d::detail::render_mission(state,request,bytes,directory,[](int,int) { return 0; });
    };
    const auto normal = render();
    const auto& player = state.entities.front().words;
    swiv3d::detail::queue_ground_effect(state.ground_effects,{{player[7],0,player[9]}},swiv3d::GroundEffectKind::teleport,39);
    const auto inactive_light = render();
    require(inactive_light.camera_position[1] == normal.camera_position[1] + 900,
            "Active spatial effect lost the original elevated camera");
    state.ground_effects.records[state.ground_effects.active].lifetime = 32;
    const auto active_light = render();
    require(active_light.camera_position == inactive_light.camera_position &&
            active_light.terrain_pixels != inactive_light.terrain_pixels,
            "Teleport lighting did not reach the terrain renderer independently of the camera");
}

void test_ground_profile_initialization() {
    const auto archive = level(2000,64,128,TestModel::point,TestActors::scenery);
    swiv3d::FrontendState request;
    request.phase = swiv3d::FrontendPhase::mission_request;
    request.player = 0;
    request.mission_profile[44] = 1;
    request.flight_attitude = {18432,-900,120};
    request.weapon_progress = 40;
    request.mission_profile[48] = 240; request.mission_profile[49] = 1;
    std::vector<std::uint8_t> config(128);
    put(config,0,20); put(config,8,2); put(config,12,1);
    swiv3d::Mission mission(archive,request,config);
    const auto state = mission.state();
    require(state.weapons.progress == request.weapon_progress,"Mission startup reset retained launcher progress");
    require(state.flight.craft == swiv3d::CraftKind::buggy && state.entities.front().words[19] == 991,
            "Ground checkpoint did not restore the selected craft");
    for (int i = 0; i < 4; ++i) {
        const auto slot = state.weapons.mounts[3 + i];
        const auto wheel = std::find_if(state.entities.begin(),state.entities.end(),[&](const auto& actor) { return actor.slot == slot; });
        require(wheel != state.entities.end() && wheel->words[19] == 980 && wheel->words[20] == i && wheel->words[23] == state.player,
                "Ground checkpoint lost wheel allocation order or ownership");
    }
    mission.advance({});
    require(mission.state().flight.attitude == request.flight_attitude,"Ground startup reset retained flight accumulators");
    request.mission_profile[44] = 2;
    request.mission_profile[48] = request.mission_profile[49] = 0;
    swiv3d::Mission ski(archive,request,config);
    require(ski.state().entities.size() == state.entities.size() - 1 && ski.state().entities.at(3).words[19] == 983,
            "Ski checkpoint did not restore its three distinct wheels");
    for (int tick = 0; tick < 36; ++tick) { ski.advance({}); }
    const auto result = ski.result();
    require(result.outcome == swiv3d::MissionOutcome::failed && result.profile[44] == 2 &&
            result.flight_attitude == request.flight_attitude,"Ground result lost craft or retained accumulators");
    require(result.weapon_progress == request.weapon_progress,"Mission result lost retained launcher progress");
    ski.restart();
    require(ski.state().tick == 0 && ski.state().entities.size() == state.entities.size() - 1 &&
            ski.state().weapons.mounts[3] == 2999 && ski.state().weapons.mounts[5] == 2997,
            "Ground restart retained stale wheel ownership");
    require(mission.resource(10000) == mission.resource(10800),"Missing model resource lost its canonical fallback alias");
}

void test_submarine_shared_model_bound() {
    const auto archive = level(5501,64,128,TestModel::point,TestActors::scenery);
    swiv3d::FrontendState request;
    request.phase = swiv3d::FrontendPhase::mission_request; request.player = 0;
    request.mission_profile[48] = 240; request.mission_profile[49] = 1;
    std::vector<std::uint8_t> config(128);
    put(config,0,20); put(config,8,2); put(config,12,1);
    swiv3d::Mission mission(archive,request,config);
    require(mission.resource(10800)[12] == 0,"Model fixture did not start with its resource bound");
    mission.advance({});
    const auto model = mission.resource(10800);
    require(model[12] == 0xd0 && model[13] == 7 && mission.resource(10000) == model,
            "Submarine model mutation bypassed canonical resource aliases");
}

void test_boss_initialization() {
    const auto archive = level(8000,64,128,TestModel::point,TestActors::scenery);
    swiv3d::FrontendState request;
    request.phase = swiv3d::FrontendPhase::mission_request; request.player = 0;
    request.mission_profile[48] = 240; request.mission_profile[49] = 1;
    std::vector<std::uint8_t> config(128);
    put(config,0,20); put(config,8,2); put(config,12,1);
    swiv3d::Mission mission(archive,request,config);
    const auto state = mission.state();
    require(state.entities.size() == 14 && state.entities.at(1).words[2] == 600,
            "Boss setup did not allocate its core and five paired gun assemblies");
    for (int part = 0; part < 5; ++part) {
        const auto& base = state.entities.at(2 + part * 2).words;
        const auto& top = state.entities.at(3 + part * 2).words;
        require(base[20] == part * 256 && base[23] == 2999 && top[23] == state.entities.at(2 + part * 2).slot &&
                top[2] == 400 && base[13] == part * 205,"Boss assembly ownership or heading changed");
    }
}

void test_mars_boss_arrival() {
    const auto original = level(8001,64,128,TestModel::point,TestActors::scenery);
    const auto arrival = scenario::arrival_start(original,scenario::Arrival::boss);
    const auto commands = arrival.load(105);
    require(commands.at(4) == 192 && commands.at(8) == 128,
            "Pentagonal boss arrival did not retain its level command coordinates");
}

void test_lunar_initialization() {
    swiv3d::FrontendState request;
    request.phase = swiv3d::FrontendPhase::mission_request; request.player = 0;
    request.mission_profile[48] = 240; request.mission_profile[49] = 1;
    std::vector<std::uint8_t> config(128);
    put(config,0,20); put(config,8,2); put(config,12,1);
    for (const auto& entry : std::array<std::pair<int,int>,6>{{{4100,5},{4101,4},{5003,4},{5010,5},{6200,6},{7020,4}}}) {
        swiv3d::Mission mission(level(entry.first,64,128,TestModel::point,TestActors::scenery),request,config);
        const auto state = mission.state();
        require(state.entities.size() == static_cast<std::size_t>(entry.second),"Lunar setup lost its actor allocation order");
        if (entry.first == 7020) {
            require(state.entities.at(1).words[20] == 5 && state.objectives == 1,
                    "Ground exit lost its armed phase or objective");
        }
    }
}

void test_mars_initialization() {
    swiv3d::FrontendState request;
    request.phase = swiv3d::FrontendPhase::mission_request; request.player = 0;
    request.mission_profile[48] = 240; request.mission_profile[49] = 1;
    std::vector<std::uint8_t> config(128);
    put(config,0,20); put(config,8,2); put(config,12,1);
    for (const auto& entry : std::array<std::pair<int,int>,5>{{{3100,5},{5004,7},{8010,6},{8020,12},{3200,4}}}) {
        swiv3d::Mission mission(level(entry.first,64,128,TestModel::point,TestActors::scenery),request,config);
        require(mission.state().entities.size() == static_cast<std::size_t>(entry.second),"Mars setup lost its actor assemblies");
    }
}

}

int main() {
    try {
        test_initialization_and_render();
        test_demo_initialization();
        test_frame_counter_boundary();
        test_flight_grid_crossing();
        test_restart_resets_owned_world();
        test_tether_line_rendering();
        test_beam_texture_clock();
        test_terrain_filter_alias();
        test_water_texture_animation();
        test_spatial_effect_rendering();
        test_ground_profile_initialization();
        test_submarine_shared_model_bound();
        test_boss_initialization();
        test_mars_boss_arrival();
        test_lunar_initialization();
        test_mars_initialization();
        std::cout << "Mission initialization and rendering tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
