#include "combat.hpp"
#include "combat_math.hpp"

namespace swiv3d::detail {
namespace {
using namespace combat_math;
constexpr int scan_radius = 6000000;
constexpr int node_arrival = 2000;
}

void Combat::later_boss(EntityState& actor) {
    auto& w = actor.words;
    const auto behavior = static_cast<Behavior>(w[26]);
    const auto node_velocity = [&](int speed, int y) {
        const auto& node = nodes_.at(w[16]);
        const int dx = node[4] - shift(w[7],8), dz = node[5] - shift(w[9],8);
        state_.object_direction = {dx, dz};
        state_.node_range = dx * dx + y * y + dz * dz;
        const auto normal = normalize({{dx,y,dz}});
        for (int axis = 0; axis < 3; ++axis) { w[10 + axis] = normal[axis] * speed; }
    };
    const auto killed = [&] {
        w[1] = 0; ground_blast(actor); debris(actor); award(actor,10000); check_objective(actor);
    };
    if (behavior == Behavior::boss2_core) {
        if (w[5] > 0) { --w[5]; }
        if (w[16] == -1) { nearest_node(actor,NodeKind::air); }
        if (w[1] == -1) { ++nodes_.at(w[16])[7]; return; }
        scan_player(actor,scan_radius); face(actor,4); node_velocity(8,0);
        const bool pursuing = w[22] != actor.slot;
        balance_height(actor,pursuing ? 50 + shift(sine(w[6] * 16),3) : 80);
        if (pursuing) {
            w[21] = 121; fire(actor);
            if (state_.boss_destroyed == 2 && (w[6] & 63) == 0) {
                const int cooldown = w[5];
                w[21] = 408; w[5] = 0; w[13] -= 80; fire(actor);
                w[5] = 0; w[13] += 160; fire(actor);
                w[5] = cooldown; w[13] -= 80;
            }
        }
        if (state_.node_range < node_arrival) { change_node(actor,NodeKind::air); }
        if (w[2] < 1) { ++nodes_.at(w[16])[7]; killed(); }
        return;
    }
    if (behavior == Behavior::boss2_top) {
        auto& parent = entity(w[23]);
        if (w[5] > 0) { --w[5]; }
        w[22] = parent.words[22] == parent.slot ? actor.slot : parent.words[22];
        if (w[20] == 0 || w[20] == 1) { place(parent,actor,w[20] == 0 ? -22000 : 22000,4000,3000); }
        if (w[22] != actor.slot) { w[21] = state_.boss_destroyed == 0 ? 110 : 111; fire(actor); }
        w[14] = parent.words[14]; w[13] = parent.words[13];
        if (w[2] < 1) {
            parent.words[20] = (parent.words[20] & 0xff00) + 10;
            if (++state_.boss_destroyed == 2) { parent.words[1] = w[1]; }
            w[1] = 0; effect(actor,Behavior::explosion,27);
        }
        return;
    }
    if (behavior == Behavior::pyramid_eye) {
        scan_player(actor,scan_radius);
        if (w[16] == -1) { nearest_node(actor,NodeKind::air); ++nodes_.at(w[16])[7]; }
        if (w[22] != actor.slot && nodes_.at(w[16])[7] > 0) { spawn(actor,0x18,50,10001,Behavior::pyramid_shot); }
        if (w[2] < 1) { killed(); ++state_.boss_destroyed; }
        return;
    }
    if (behavior == Behavior::pyramid_shot || behavior == Behavior::pyramid_dive) {
        if (behavior == Behavior::pyramid_shot) {
            if (w[5] > 0) { --w[5]; }
            if (w[16] == -1) { nearest_node(actor,NodeKind::air); }
            if (w[1] == -1) { ++nodes_.at(w[16])[7]; return; }
            const auto& node = nodes_.at(w[16]);
            const int dy = shift(height_(node[4] * 256,node[5] * 256) + 32000,8) - shift(w[8],8);
            node_velocity(16,dy);
            if (state_.node_range < node_arrival) {
                const int previous = w[16]; change_node(actor,NodeKind::air);
                if (w[16] != previous) { w[6] = 0; }
                else {
                    const auto& target = entity(w[22]).words;
                    const int dx = shift(target[7],8) - shift(w[7],8), dz = shift(target[9],8) - shift(w[9],8);
                    state_.node_range = dx * dx + dz * dz;
                    if (w[6] > 50 || state_.node_range < 200000) {
                        const auto direction = normalize({{dx,shift(target[8],8) - shift(w[8],8),dz}});
                        for (int axis = 0; axis < 3; ++axis) { w[10 + axis] = direction[axis] * 32; }
                        w[26] = static_cast<int>(Behavior::pyramid_dive); ++nodes_.at(w[16])[7];
                    }
                }
            }
        }
        if (w[2] < 1) { ++nodes_.at(w[16])[7]; w[1] = 0; effect(actor,Behavior::explosion,27); }
        return;
    }
    if (behavior == Behavior::mars_face) {
        if (w[16] == -1) { nearest_node(actor,NodeKind::air); ++nodes_.at(w[16])[7]; }
        if (w[6] > 40 && nodes_.at(w[16])[7] > 0) {
            const auto command = resource_(105);
            auto& drone = spawn(actor,0x1001,110,word(command,w[25] + 20),Behavior::slug_drone);
            drone.words[3] = word(command,w[25] + 32); drone.words[22] = drone.slot;
            drone.words[21] = word(command,w[25] + 28); w[6] = 0;
        }
        return;
    }
    if (behavior == Behavior::pyramid_gun) {
        if (w[5] > 0) { --w[5]; }
        const int phase = w[20];
        auto& parent = entity(w[23]).words;
        if (phase == 0 || phase == 1) {
            w[7] = parent[7]; w[9] = parent[9];
            if (phase == 1) {
                w[8] = parent[8] - 20000; w[22] = parent[22]; face(actor,4); w[21] = 510; fire(actor);
                if (parent[1] == 0) { w[1] = 0x1010; w[23] = actor.slot; ++w[20]; }
            }
        } else if (phase == 2) {
            w[10] = shift((w[10] + sine(w[13])) * 14,4); w[12] = shift((w[12] + cosine(w[13])) * 14,4);
            w[22] = entity(w[23]).words[22]; face(actor,4); w[21] = 510; fire(actor);
            balance_height(actor,80 + shift(sine(w[6] * 16),3));
            if (w[2] < 1) { w[1] = 0; effect(actor,Behavior::explosion,27); debris(actor); }
        }
        return;
    }
    scan_player(actor,scan_radius);
    if (w[16] == -1) { nearest_node(actor,NodeKind::air); }
    if (w[20] == 4) { w[6] = 0; ++w[20]; }
    else if (w[20] == 5 || w[20] == 6) {
        w[8] += 500;
        if (w[20] == 6) { w[1] = 0x1010; }
        if (w[6] > 39) {
            w[6] = 0;
            if (w[20]++ == 5) {
                const auto command = resource_(105);
                auto& gun = spawn(actor,0x40,100,word(command,w[25] + 20),Behavior::pyramid_gun);
                w[23] = gun.slot; gun.words[20] = 0;
            } else { entity(w[23]).words[20] = 1; }
        }
    } else if (w[20] == 7) {
        w[13] += 4; node_velocity(8,0); balance_height(actor,110 + shift(sine(w[6] * 16),3));
        if (state_.node_range < node_arrival) { change_node(actor,NodeKind::air); }
    }
    if (w[2] < 1) { killed(); }
}

}
