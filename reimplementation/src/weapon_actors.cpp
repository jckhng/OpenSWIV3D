#include "combat.hpp"
#include "combat_math.hpp"
#include "combat_jitter.hpp"
#include "ground_effects.hpp"

namespace swiv3d::detail {
namespace {
using namespace combat_math;
}

void Combat::trail(EntityState& actor, int interval) {
    if (actor.words[6] <= interval) { return; }
    auto& smoke = spawn(actor, 0xe1, 100, 11001, Behavior::fire_trail);
    smoke.words[10] = smoke.words[11] = smoke.words[12] = 0;
    smoke.words[20] = 13;
    actor.words[6] = 0;
}

std::vector<int> Combat::weapon_targets(EntityState& actor, int radius, int mask) {
    std::vector<int> result;
    const int x = shift(actor.words[7], 17), z = shift(actor.words[9], 17);
    const auto append = [&](int head) {
        for (int slot = head; slot != no_entity; slot = entity(slot).words[0]) {
            if ((entity(slot).words[1] & mask) == 0) { result.push_back(slot); }
        }
    };
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) { append(state_.grid[((z + dz) & 63) * 64 + ((x + dx) & 63)]); }
    }
    append(actor.words[0]);
    std::reverse(result.begin(), result.end());
    return result;
}

void Combat::search_weapon_target(EntityState& actor) {
    auto& w = actor.words;
    std::vector<int> targets;
    const int x = shift(w[7], 17), z = shift(w[9], 17);
    for (int dz = -3; dz <= 3; ++dz) {
        for (int dx = -3; dx <= 3; ++dx) {
            for (int slot = state_.grid[((z + dz) & 63) * 64 + ((x + dx) & 63)]; slot != no_entity; slot = entity(slot).words[0]) {
                if ((entity(slot).words[1] & 0x44) == 0) { targets.push_back(slot); }
            }
        }
    }
    w[22] = actor.slot;
    int best = 50000;
    for (auto it = targets.rbegin(); it != targets.rend(); ++it) {
        const auto& target = entity(*it).words;
        const auto direction = normalize({{shift(target[7] - w[7], 8),0,shift(target[9] - w[9], 8)}});
        const int bias = (target[1] & 0x1000) != 0 ? 4000 : (target[1] & 8) != 0 ? -6000 : (target[1] & 0x4000) != 0 ? -4000 : 0;
        const int score = sine(w[13]) * direction[0] + cosine(w[13]) * direction[2] + bias;
        if (score > best) { best = score; w[22] = *it; }
    }
    w[6] = 0;
}

void Combat::weapon_actor(EntityState& actor) {
    auto& w = actor.words;
    const auto behavior = static_cast<Behavior>(w[26]);
    const auto impact = [&] {
        if (w[8] <= height_(w[7], w[9]) || w[2] < 1) { w[1] = 0; effect(actor, Behavior::bang, 39); }
    };
    const auto hover = [&](int clearance, int down_force) {
        const int ground = std::max(height_(w[7], w[9]), height_(w[7] + w[10] * 32, w[9] + w[12] * 32));
        const int error = shift(ground, 8) + clearance - shift(w[8], 8);
        w[11] += error * (error > 0 ? 4 : down_force);
        w[11] = shift(w[11] * 29, 5);
    };
    const auto fragments = [&](int count, int scale) {
        for (int i = 0; i < count; ++i) {
            auto& bullet = spawn(actor, 0x3f, 15, 10005, Behavior::falling_bullet);
            bullet.words[5] = 50 + i * 2;
            const auto vector = combat_jitter[state_.effects_phase];
            for (int axis = 0; axis < 3; ++axis) { bullet.words[10 + axis] = vector[axis] * scale; }
            state_.effects_phase = (state_.effects_phase + 1) & 255;
        }
    };
    const auto burst = [&](int radius_squared) {
        effect(actor, Behavior::aircraft_explosion, 13);
        for (int slot : weapon_targets(actor, 2)) {
            const auto& target = entity(slot).words;
            auto& bullet = spawn(actor, 0x3b, 25, 10001, Behavior::enemy_bullet);
            std::array<int, 3> delta{};
            for (int axis = 0; axis < 3; ++axis) { delta[axis] = shift(target[7+axis] - w[7+axis], 8); }
            if (shift(delta[0] * delta[0] + delta[2] * delta[2], 2) < radius_squared) {
                const auto normal = normalize(delta);
                for (int axis = 0; axis < 3; ++axis) { bullet.words[10+axis] = normal[axis] * 32; }
            }
        }
        fragments(10, 8);
    };
    switch (behavior) {
    case Behavior::rocket_buddy:
    case Behavior::tracking_buddy:
    case Behavior::bomber_buddy: {
        const auto& parent = entity(w[23]).words;
        w[15] = 0;
        if (behavior == Behavior::tracking_buddy) {
            if (entity(w[22]).words[1] == 0) { w[6] = 9999; }
            search(actor,10,1,0x45,1);
        } else { w[22] = parent[22]; }
        ++w[5];
        w[10] = parent[10];
        w[12] = parent[12];
        hover(90, 2);
        if (behavior == Behavior::tracking_buddy) {
            if (w[22] == actor.slot) {
                w[13] += shift(parent[13] - w[13],1);
                w[14] = shift(w[14],1);
            } else {
                const int direction = face(actor,3);
                if (direction != 0) { w[13] &= 511; w[14] &= 511; }
                if (direction == 0 && ((w[5] & 15) == 0 || (w[5] & 15) == 3)) {
                    auto& bullet = spawn(actor,0x3f,14,10000,Behavior::bullet);
                    bullet.words[5] = 30;
                    bullet.words[23] = w[23];
                    const auto motion = rotate(bullet,{{0,0,9000}});
                    std::copy(motion.begin(),motion.end(),bullet.words.begin() + 10);
                }
            }
        } else if (behavior == Behavior::rocket_buddy) {
            if (w[22] == w[23]) {
                w[13] += shift(parent[13] - w[13],1);
                w[14] = shift(w[14],1);
            } else { face(actor,3); }
            const int phase = w[5] & 31;
            if (state_.weapons.primary_held != 0 && (phase == 0 || phase == 7)) {
                constexpr int rocket_speed = 9000, rocket_spread = 150;
                for (int side : {-rocket_spread,rocket_spread}) {
                    auto& bullet = spawn(actor,0x3f,8,10002,Behavior::bullet);
                    bullet.words[5] = 30;
                    bullet.words[23] = w[23];
                    const auto motion = rotate(bullet,phase == 7 ? std::array<int,3>{side,0,rocket_speed} :
                                                                 std::array<int,3>{0,side,rocket_speed});
                    std::copy(motion.begin(),motion.end(),bullet.words.begin() + 10);
                }
            }
        } else {
            w[13] = parent[13];
            w[14] = 0;
            if (state_.weapons.primary_held != 0 && (w[5] & 31) == 0) {
                auto& bomb = spawn(actor, 0x3f, 20, 10001, Behavior::buddy_bomb);
                bomb.words[5] = 20;
                bomb.words[23] = w[23];
            }
        }
        if (w[2] < 1) { w[1] = 0; effect(actor, Behavior::explosion, 27); }
        break;
    }
    case Behavior::buddy_bomb:
        impact();
        velocity(actor, 7000);
        if (entity(w[22]).words[1] == 0) { w[22] = w[23]; }
        if (w[22] != w[23] && w[22] != actor.slot && w[6] < 6) {
            face(actor, 2);
            w[15] = 0;
        }
        trail(actor, 3);
        break;
    case Behavior::polaris_missile: {
        --w[5]; impact(); trail(actor, 3);
        const int mode = w[20];
        const auto home = [&](int speed, int turn) {
            velocity(actor, speed);
            if (entity(w[22]).words[1] == 0) { w[22] = w[23]; }
            if (w[22] != w[23] && w[22] != actor.slot && w[6] < 6) { face(actor, turn); w[15] = 0; }
        };
        if (mode == 0 || mode == 10 || mode == 20) {
            const auto& target = entity(w[22]).words;
            steer_towards(actor, target[7], target[9], 5);
            if (w[5] < 1) { w[5] = 13; ++w[20]; }
        } else if (mode == 1 || mode == 11 || mode == 21) {
            home(900, 14);
            if (w[5] < 1) { w[5] = 13; ++w[20]; }
        } else if (mode == 2) {
            home(900, 2);
            if (w[5] < 1) { ++w[20]; home(5000, 2); }
        } else if (mode == 12 || mode == 22) {
            home(900, 2);
            if (w[5] < 1) { w[5] = 150; ++w[20]; }
        } else if (mode == 13 || mode == 23) {
            home(5000, mode == 13 ? 1 : 4);
            if (w[5] < 1) { ++w[20]; }
        }
        break;
    }
    case Behavior::falling_casing:
        w[11] -= 20; impact();
        w[13] += w[20] == 0 ? 5 : -5;
        w[15] += w[20] == 0 ? 6 : -6; w[14] += 3;
        break;
    case Behavior::bomblet:
        if (w[8] <= height_(w[7], w[9])) {
            w[19] = 901; w[1] = 0;
            const int size = bound(w[19]);
            const int damage = size < 1000 ? 0 : size < 5000 ? 1 : 2;
            if (deform_) { deform_(actor, damage); }
            add_object(actor, 0xe1, 3000, 900 + damage, Behavior::ground_explosion);
            illuminate(actor, damage);
        }
        if (w[2] != 50) { w[1] = 0; effect(actor, Behavior::bang, 39); }
        w[11] -= 90;
        break;
    case Behavior::cruise_missile: {
        impact(); w[14] = w[15] = 0; hover(90, 2);
        w[10] = shift(sine(w[13]) * w[5], 8); w[12] = shift(cosine(w[13]) * w[5], 8);
        w[14] = std::clamp(shift(w[11], 5), -70, 70);
        if (w[20] == 10 || w[20] == 11 || w[20] == 20 || w[20] == 21) {
            const bool left = (w[20] & 1) == 0;
            const int turn = w[20] < 20 ? 6 : 8;
            w[13] += (w[6] < 10 ? turn : -turn / 2) * (left ? 1 : -1);
            if (w[6] > 30) { w[20] = w[20] < 20 ? 30 : 22; }
        } else if (w[20] == 22) {
            w[10] = shift(w[10] * 14, 4); w[12] = shift(w[12] * 14, 4);
            if (state_.weapons.primary_held != 0) { w[20] = 40; w[22] = entity(w[23]).words[22]; }
        } else if (w[20] == 30 || w[20] == 40) {
            trail(actor, 3);
            if (w[20] == 40 && w[22] == w[23]) { w[20] = 41; }
        }
        else if (w[20] == 41) {
            if (w[23] == w[22]) { w[22] = entity(w[23]).words[22]; }
            if (w[23] != w[22]) { w[20] = 40; }
            w[10] = shift(w[10] * 14, 4); w[12] = shift(w[12] * 14, 4);
        }
        if (w[20] != 30 && w[20] != 40) { break; }
        const auto& target = entity(w[22]).words;
        if (w[22] == w[23]) { w[5] += 10; if (w[20] == 40) { w[22] = entity(w[23]).words[22]; } break; }
        const int dx = shift(target[7] - w[7], 5), dz = shift(target[9] - w[9], 5);
        state_.object_direction = {dx, dz};
        constexpr int turns[]{0,-1,1,-5,5,-15,15};
        constexpr int acceleration[]{20,-3,3,0,0,-20,-20};
        int best = -99999999, selected = 0;
        for (int i = 0; i < 7; ++i) {
            const int product = dx * sine(w[13] + turns[i]) + dz * cosine(w[13] + turns[i]);
            if (product > best) { best = product; selected = i; }
        }
        if (dx * dx + dz * dz < 90000) { w[11] += shift(target[8] - w[8], 6); }
        w[13] += turns[selected]; w[5] += acceleration[selected]; w[15] = turns[selected] * 8;
        if (target[1] == 0) { w[22] = w[23]; }
        break;
    }
    case Behavior::shard_bomb:
    case Behavior::pulse_ring:
    case Behavior::pulse_wall:
        if (behavior != Behavior::shard_bomb && w[20] != 0) {
            if (--w[5] < 1) { w[1] = 0; }
            if ((w[5] & 1) != 0) { break; }
            for (int slot : weapon_targets(actor, 4, 0x44)) {
                auto& target = entity(slot).words;
                const int dx = shift(target[7], 8) - shift(w[7], 8), dz = shift(target[9], 8) - shift(w[9], 8);
                const int angle = behavior == Behavior::pulse_ring ? shift(dx * dx + dz * dz, 9) + (w[5] - 20) * 64 :
                    (w[5] - 30) * 16 + shift(dx, 2) + 128;
                if (angle <= 0 || angle >= 255 || (behavior == Behavior::pulse_wall && shift(dx, 2) * shift(dx, 2) + shift(dz, 2) * shift(dz, 2) >= 50000)) { continue; }
                const int damage = behavior == Behavior::pulse_ring ? shift(sine(angle) * w[5], 6) : shift(sine(angle), 2);
                target[2] = std::max(0, target[2] - damage); target[1] = (target[1] | 0x20000) & 0x7ffff;
            }
            break;
        }
        w[14] = behavior == Behavior::shard_bomb ? 0 : behavior == Behavior::pulse_ring ? 20 : 40;
        w[15] = 0;
        if (behavior != Behavior::shard_bomb) { w[13] += behavior == Behavior::pulse_ring ? 10 : 15; }
        w[10] = shift(w[10] * 30, 5); w[12] = shift(w[12] * 30, 5);
        if (w[6] == 0) {
            w[1] = 0x40;
            w[10] = sine(entity(w[23]).words[13]) * 32; w[12] = cosine(entity(w[23]).words[13]) * 32;
        }
        hover(behavior == Behavior::shard_bomb ? 90 : 40, 4);
        if (w[6] <= 39 || state_.ground_effects.free == no_entity) { break; }
        effect(actor, Behavior::bang, 39);
        if (behavior == Behavior::shard_bomb) {
            w[1] = 0; fragments(35, 16);
            if (w[20] == 1) { fragments(20, 8); }
        } else {
            w[19] = 0; w[5] = behavior == Behavior::pulse_ring ? 40 : 60; w[20] = 1;
            int floor = height_(w[7],w[9]);
            if (behavior == Behavior::pulse_wall) {
                const auto& owner = entity(w[23]).words;
                floor = std::min(floor,height_(owner[7],owner[9]));
            }
            queue_ground_effect(state_.ground_effects,{{w[7],floor,w[9]}},
                                behavior == Behavior::pulse_ring ? GroundEffectKind::pulse_ring : GroundEffectKind::pulse_wall,w[5]);
        }
        break;
    case Behavior::scatter_missile:
    case Behavior::cluster_missile:
        ++w[5]; impact(); w[14] -= 3;
        if (w[5] > 34) {
            if (behavior == Behavior::scatter_missile) { w[1] = 0; burst(80000); }
            else {
                w[13] += 50;
                for (int i = 0; i < 3; ++i) {
                    w[13] += 170;
                    auto& child = spawn(actor, 0x3b, 25, 10003, Behavior::cluster_child);
                    child.words[14] = 0; child.words[5] = i * 20 + 30;
                }
                w[1] = 0; effect(actor, Behavior::bang, 39);
            }
        }
        velocity(actor, 4500); trail(actor, 6);
        break;
    case Behavior::cluster_child:
        --w[5]; impact();
        if (w[5] < 1) { w[1] = 0; burst(20000); }
        velocity(actor, 1700);
        break;
    case Behavior::bomber_missile:
    case Behavior::carpet_missile:
        ++w[5]; impact();
        if (w[5] > 5 && w[5] < 12) { w[14] -= 10; }
        if (behavior == Behavior::bomber_missile) {
            if (w[5] > 17 && w[5] < 120 && (w[5] & 3) == 0) {
                auto& bomb = spawn(actor, 0x3f, 50, 10005, Behavior::bomblet);
                const auto vector = combat_jitter[state_.effects_phase];
                bomb.words[10] = vector[0]; bomb.words[11] = -600; bomb.words[12] = vector[2];
                state_.effects_phase = (state_.effects_phase + 1) & 255;
            }
            velocity(actor, 4500);
        } else {
            if (w[5] > 5 && w[5] < 12) { velocity(actor, 4500); }
            if (w[5] > 17 && w[5] < 40) {
                const auto v = rotate(actor, {{0,1000,4500}});
                std::copy(v.begin(), v.end(), w.begin() + 10);
            }
            if (w[5] > 39 && w[5] < 150 && (w[5] & 3) == 0) {
                auto& bomb = spawn(actor, 0x3f, 50, 10005, Behavior::bomblet);
                const auto vector = combat_jitter[state_.effects_phase];
                bomb.words[10] = vector[0] * 4; bomb.words[11] = -600; bomb.words[12] = vector[2] * 4;
                state_.effects_phase = (state_.effects_phase + 1) & 255;
                const auto v = rotate(actor, {{0,200,150}});
                std::copy(v.begin(), v.end(), w.begin() + 10);
            }
            if (w[5] >= 150) { velocity(actor, 4500); }
        }
        trail(actor, 6);
        break;
    default: break;
    }
}

}
