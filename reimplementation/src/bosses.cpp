#include "combat.hpp"
#include "combat_math.hpp"

namespace swiv3d::detail {
namespace {
using namespace combat_math;
constexpr int boss_parts = 5;
constexpr int boss_scan_radius = 6000000;
}

void Combat::scan_player(EntityState& actor, int radius_squared) {
    ++state_.game_counter;
    auto& w = actor.words;
    const auto& player = entity(state_.player).words;
    const int dx = shift(w[7] - player[7],8), dz = shift(w[9] - player[9],8);
    state_.object_direction[0] = dx;
    w[22] = (player[1] & 0x40) == 0 && dx * dx + dz * dz < radius_squared ? state_.player : actor.slot;
}

void Combat::turn_to(EntityState& actor, int x, int z, int turn) {
    auto& w = actor.words;
    const int dx = shift(x,8) - shift(w[7],8), dz = shift(z,8) - shift(w[9],8);
    state_.object_direction = {dx, dz};
    int best = -999999999, heading = w[13];
    for (int offset : {-turn,0,turn}) {
        const int score = sine(w[13] + offset) * dx + cosine(w[13] + offset) * dz;
        if (score > best) { best = score; heading = w[13] + offset; }
    }
    w[13] = heading;
}

void Combat::boss(EntityState& actor) {
    auto& w = actor.words;
    const auto behavior = static_cast<Behavior>(w[26]);
    if (behavior == Behavior::boss_core) {
        int phase = shift(w[20],8), part = w[20] & 255;
        scan_player(actor,boss_scan_radius);
        if (phase == 0) {
            if (w[22] != actor.slot) { phase = 1; }
            w[1] |= 0x10000;
        } else if (phase == 1) {
            if (part == boss_parts) { part = 0; if (w[22] == actor.slot) { phase = 0; } }
            w[1] |= 0x10000;
            if (state_.boss_destroyed == boss_parts) {
                w[6] = 0; part = -1; phase = 10; w[1] &= 0xeffff;
            }
        } else if (phase == 10) {
            if (w[6] == 40) { w[1] = 0x1000; }
            w[2] = 3000;
            if (w[6] == 70) { part = 0; state_.boss_volley = 0; phase = 20; w[6] = 0; }
        } else if (phase == 20) {
            if (w[6] > 13) {
                if (++state_.boss_volley > 4) { phase = 30; }
                w[6] = 0;
            }
        } else if (phase == 30) { w[6] = 0; phase = 31; }
        else if (phase == 31) {
            if (w[22] != actor.slot && (w[6] == 100 || w[6] == 130 || w[6] == 160)) {
                const auto commands = resource_(105);
                auto& aircraft = spawn(actor,0x1032,150,word(commands,w[25] + 20),Behavior::boss_air);
                aircraft.words[22] = w[22];
                aircraft.words[13] += w[6] == 130 ? 170 : w[6] == 160 ? 341 : 0;
                aircraft.words[14] = 128;
            }
            if (w[6] == 240) { state_.boss_volley = 0; phase = 20; w[6] = 0; }
        }
        w[20] = phase * 256 + (part & 255);
        if (w[2] < 1) {
            w[1] = 0; ground_blast(actor); debris(actor); award(actor,10000); check_objective(actor);
        }
        return;
    }
    if (behavior == Behavior::boss_base) {
        auto& core = entity(w[23]).words;
        w[22] = core[22] == w[23] ? actor.slot : core[22];
        const int part = shift(w[20],8), phase = w[20] & 255;
        if (phase == 0 && part + 256 == core[20]) { ++w[20]; }
        else if (phase == 2) { ++core[20]; w[20] &= 0xff00; }
        else if (phase == 10) {
            if (part + 256 == core[20]) { ++core[20]; }
            if (state_.boss_destroyed > 4) { ++w[20]; }
        } else if (phase == 11 && part == state_.boss_volley) { ++w[20]; }
        else if (phase == 12) { w[21] = 612; w[5] = 0; fire(actor); ++w[20]; }
        else if (phase == 13 && part != state_.boss_volley) { w[20] -= 2; }
        return;
    }
    if (behavior == Behavior::boss_top) {
        auto& base = entity(w[23]).words;
        w[22] = base[22] == w[23] ? actor.slot : base[22];
        const int phase = w[20];
        if (phase == 0) {
            if ((base[20] & 255) == 1) { w[6] = 0; w[20] = 1; }
        } else if (phase == 1) {
            w[14] += 7; w[8] += 1300;
            if (w[6] == 20) { w[20] = 2; w[5] = 15; w[6] = 0; w[21] = 600 + (state_.boss_destroyed > 1); }
            if (w[6] == 3) { w[1] = 0x1000; }
        } else if (phase == 2) {
            if (w[6] == 45) { w[6] = 0; w[20] = 3; }
            if (w[5] > 0) { --w[5]; }
            fire(actor);
        } else if (phase == 3) {
            w[14] -= 7; w[8] -= 1300;
            if (w[6] == 20) { w[20] = 0; }
            if (w[6] == 17) { w[1] = 0x40; }
            if (w[6] == 5) { base[20] = (base[20] & 0xff00) + 2; }
            w[22] = base[23];
        }
        if (phase >= 1 && phase <= 3) {
            const auto& target = entity(w[22]).words;
            turn_to(actor,target[7],target[9],phase == 2 ? 5 : 10);
        }
        if (w[2] < 1) {
            ++state_.boss_destroyed; base[20] = (base[20] & 0xff00) + 10;
            w[1] = 0; effect(actor,Behavior::explosion,27);
        }
        return;
    }
    int speed = 1000;
    if (w[20] == 0) {
        if (w[6] == 15) { w[6] = 0; w[20] = 1; }
    } else if (w[20] == 1) {
        if (w[6] == 32) { w[20] = 2; }
        w[14] -= 4; speed += w[6] * 16; w[13] += 2;
    } else if (w[20] == 2) {
        if (w[6] == 60) { w[20] = 3; }
        w[15] = 50; speed += w[6] * 16; w[13] += 3;
    } else if (w[20] == 3) {
        state_.object_direction[0] = w[6] & 15;
        w[21] = 600;
        if ((w[6] & 15) == 0 || (w[6] & 15) == 3 || (w[6] & 15) == 7) {
            if (w[5] > 0) { --w[5]; }
            fire(actor);
        }
        if (w[6] == 130) { w[20] = 4; }
        w[15] = 50; speed += w[6] * 16; w[13] += 2;
    } else if (w[20] == 4) {
        if (w[8] <= height_(w[7],w[9]) || w[2] < 1) {
            w[1] = 0; effect(actor,Behavior::bang,39); debris(actor);
        }
        constexpr std::array<std::array<int,3>,5> vectors{{{{0,0,1000}},{{-44,0,999}},{{44,0,999}},{{0,44,999}},{{0,-44,999}}}};
        const auto& target = entity(w[22]).words;
        state_.object_direction = {target[7] - w[7], target[9] - w[9]};
        int best = -999999999, selected = 0;
        for (int i = 0; i < 5; ++i) {
            const auto direction = rotate(actor,vectors[i]);
            std::int64_t product = 0;
            for (int axis = 0; axis < 3; ++axis) { product += static_cast<std::int64_t>(direction[axis]) * (target[7+axis] - w[7+axis]); }
            const auto score = static_cast<std::int32_t>(static_cast<std::uint32_t>(product));
            if (score > best) { best = score; selected = i; }
        }
        if (selected == 0) { if (w[15] > 0) { w[15] -= 4; } if (w[15] < 0) { w[15] += 4; } }
        if (selected == 1) { w[13] -= 5; if (w[15] > -40) { w[15] -= 5; } }
        if (selected == 2) { w[13] += 5; if (w[15] < 40) { w[15] += 5; } }
        if (selected == 3) { w[14] += 2; }
        if (selected == 4) { w[14] -= 2; }
        speed = 4000;
    }
    velocity(actor,speed);
    if (w[2] < 1) { w[1] = 0; effect(actor,Behavior::bang,39); debris(actor); award(actor,59); }
}

}
