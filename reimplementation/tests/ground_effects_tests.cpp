#include "../src/ground_effects.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

void test_pool() {
    swiv3d::GroundEffects effects;
    swiv3d::detail::queue_ground_effect(effects,{{100,200,300}},swiv3d::GroundEffectKind::pulse_ring,2);
    require(effects.active == 4 && effects.free == 3,"Ground effect did not allocate the original last free slot");
    swiv3d::detail::queue_ground_effect(effects,{{400,500,600}},swiv3d::GroundEffectKind::pulse_wall,3);
    require(effects.active == 3 && effects.records[3].next == 4,"Ground effect did not prepend its active record");
    swiv3d::detail::advance_ground_effects(effects);
    require(effects.active == 4 && effects.records[4].next == 3 && effects.records[4].lifetime == 1,
            "Ground effect update lost the original reversed active order");
    swiv3d::detail::advance_ground_effects(effects);
    require(effects.active == 3 && effects.records[3].next == -1 && effects.free == 4,
            "Expired ground effect did not return to the free list");
    swiv3d::detail::queue_ground_effect(effects,{{700,800,900}},swiv3d::GroundEffectKind::teleport,40);
    require(effects.active == 4 && effects.records[4].position[1] == 200,
            "Teleport lost the original unused Y value on pool reuse");
    require(effects.records[4].position[0] == 700 && effects.records[4].position[2] == 900,
            "Teleport lost its arrival position");
}

void test_samples() {
    swiv3d::GroundEffects effects;
    swiv3d::detail::queue_ground_effect(effects,{{0,0,0}},swiv3d::GroundEffectKind::teleport,32);
    int height = 100, shade = 500, texture = 64;
    swiv3d::detail::apply_ground_effects(effects,0,0,height,shade,texture);
    require(height == 100 && shade == 319 && texture == 64,"Teleport lighting disagrees with the retained CPU sample");
    effects = {};
    swiv3d::detail::queue_ground_effect(effects,{{0,0,0}},swiv3d::GroundEffectKind::pulse_ring,21);
    height = 1000; shade = 50;
    swiv3d::detail::apply_ground_effects(effects,0,0,height,shade,texture);
    require(height == 882 && shade == 148 && texture == 64,"Pulse ring lost its terrain displacement or lighting");
    effects = {};
    swiv3d::detail::queue_ground_effect(effects,{{0,0,0}},swiv3d::GroundEffectKind::pulse_wall,30);
    height = 0; shade = 50;
    swiv3d::detail::apply_ground_effects(effects,0,0,height,shade,texture);
    require(height == 212 && shade == 1000 && texture == 0,"Shockwall lost its height, light or texture override");
    height = 300; shade = 50; texture = 64;
    swiv3d::detail::apply_ground_effects(effects,0,0,height,shade,texture);
    require(height == 300 && shade == 50 && texture == 64,"Shockwall lowered terrain above its profile");
}

void test_capacity_and_expiry() {
    swiv3d::GroundEffects effects;
    for (std::size_t i = 0; i < swiv3d::ground_effect_slots; ++i) {
        swiv3d::detail::queue_ground_effect(effects,{{0,0,0}},swiv3d::GroundEffectKind::pulse_ring,1);
    }
    require(effects.free == swiv3d::no_entity,"Ground-effect capacity exceeded the five original slots");
    bool rejected = false;
    try { swiv3d::detail::queue_ground_effect(effects,{{1,2,3}},swiv3d::GroundEffectKind::pulse_wall,1); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected && effects.active == 0,"Exhausted effect allocation changed the active list");
    swiv3d::detail::advance_ground_effects(effects);
    require(effects.active == swiv3d::no_entity && effects.free == 4,
            "Expired effect records did not restore the original free order");
    int height = 100, shade = 50, texture = 64;
    swiv3d::detail::apply_ground_effects(effects,0,0,height,shade,texture);
    require(height == 100 && shade == 50 && texture == 64,"Expired effects still changed the terrain sample");
}

void test_boundaries_and_order() {
    swiv3d::GroundEffects effects;
    swiv3d::detail::queue_ground_effect(effects,{{0,0,0}},swiv3d::GroundEffectKind::pulse_ring,21);
    int height = 1000, shade = 50, texture = 64;
    swiv3d::detail::apply_ground_effects(effects,65536,0,height,shade,texture);
    require(height == 882 && shade == 148,"Pulse radius lost the original 32-bit square wrap");
    effects = {};
    swiv3d::detail::queue_ground_effect(effects,{{0,0,0}},swiv3d::GroundEffectKind::pulse_wall,30);
    height = 0; shade = 50; texture = 64;
    swiv3d::detail::apply_ground_effects(effects,508,0,height,shade,texture);
    require(height == 0 && shade == 50 && texture == 64,"Shockwall included its excluded phase-255 boundary");
    effects = {};
    swiv3d::detail::queue_ground_effect(effects,{{0,0,0}},swiv3d::GroundEffectKind::teleport,25);
    swiv3d::detail::apply_ground_effects(effects,1024,0,height,shade,texture);
    require(height == 0 && shade == 50 && texture == 64,"Teleport included its excluded phase-512 boundary");
    effects = {};
    swiv3d::detail::queue_ground_effect(effects,{{0,0,0}},swiv3d::GroundEffectKind::pulse_ring,21);
    swiv3d::detail::queue_ground_effect(effects,{{0,0,0}},swiv3d::GroundEffectKind::pulse_wall,30);
    swiv3d::detail::apply_ground_effects(effects,0,0,height,shade,texture);
    require(height == 94 && shade == 1098 && texture == 0,"Overlapping effects lost their active-list application order");
}

void replay_samples(const char* input_path, const char* output_path) {
    std::ifstream input(input_path);
    std::ofstream output(output_path);
    std::string format;
    if (!(input >> format) || format != "swiv3d-ground-effect-samples-v1") { throw std::runtime_error("Invalid effect samples"); }
    int count;
    while (input >> count) {
        if (count < 0 || count > static_cast<int>(swiv3d::ground_effect_slots)) { throw std::runtime_error("Invalid effect count"); }
        swiv3d::GroundEffects effects;
        for (int i = 0; i < count; ++i) {
            std::array<std::int32_t,3> position{};
            int kind, lifetime;
            if (!(input >> position[0] >> position[1] >> position[2] >> lifetime >> kind)) { throw std::runtime_error("Truncated effect record"); }
            swiv3d::detail::queue_ground_effect(effects,position,static_cast<swiv3d::GroundEffectKind>(kind),lifetime);
        }
        int x, z, height, shade, texture;
        if (!(input >> x >> z >> height >> shade >> texture)) { throw std::runtime_error("Truncated terrain sample"); }
        swiv3d::detail::apply_ground_effects(effects,x,z,height,shade,texture);
        output << height << ' ' << shade << ' ' << texture << '\n';
    }
    if (!input.eof() || !output) { throw std::runtime_error("Cannot replay effect samples"); }
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3) { replay_samples(argv[1],argv[2]); return 0; }
        if (argc != 1) { throw std::runtime_error("Expected optional INPUT OUTPUT"); }
        test_pool();
        test_samples();
        test_capacity_and_expiry();
        test_boundaries_and_order();
        std::cout << "Ground effect pool and spatial samples passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
