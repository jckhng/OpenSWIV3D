#include "combat.hpp"
#include "combat_math.hpp"

namespace swiv3d::detail {
namespace {
using namespace combat_math;
constexpr int player_scan_radius = 6000000;

std::array<int,2> drone_thrust(int heading) {
    constexpr int angles = 512, extended_scale = 320;
    constexpr int vector_scale = 255, vector_y = 16, vector_halfwords = 4;
    int s = sine(heading), c = cosine(heading);
    // This callback indexes the adjacent norm128/sin256/sin320/cos256/cos320
    // tables before its gun normalizes the heading on the next update.
    if (heading < 0) {
        const int halfword = angles * vector_halfwords + heading;
        const int entry = halfword / vector_halfwords, component = halfword % vector_halfwords;
        int value = 0;
        if (entry != angles - 1) {
            const int x = vector_scale - entry;
            const double length = std::sqrt(x * x + double(vector_y * vector_y));
            value = static_cast<int>(vector_scale * (component < 2 ? x : vector_y) / length);
        }
        const auto bits = static_cast<std::uint32_t>(value) >> ((component & 1) * 16);
        s = static_cast<std::int16_t>(bits & 0xffff);
        c = static_cast<int>(std::round(extended_scale * std::sin(heading * 2 * pi / angles)));
    } else if (heading >= angles) {
        s = static_cast<int>(std::round(extended_scale * std::sin((heading - angles) * 2 * pi / angles)));
        c = static_cast<int>(std::round(extended_scale * std::cos((heading - angles) * 2 * pi / angles)));
    }
    return {{s,c}};
}
}

void Combat::balance_height(EntityState& actor, int clearance) {
    auto& w = actor.words;
    const int ground = std::max(height_(w[7],w[9]),height_(w[7] + w[10] * 32,w[9] + w[12] * 32));
    const int error = shift(ground,8) + clearance - shift(w[8],8);
    w[11] = shift(w[11] * 14,4);
    if (error > 0) { w[11] += error * 2; }
    if (error < 0) { w[11] += shift(error,2); }
}

void Combat::follow_node(EntityState& actor, int turn) {
    auto& w = actor.words;
    const auto& node = nodes_.at(w[16]);
    const int dx = node[4] - shift(w[7],8), dz = node[5] - shift(w[9],8);
    state_.object_direction = {dx, dz};
    state_.node_range = dx * dx + dz * dz;
    turn_to(actor,node[4] * 256,node[5] * 256,turn);
}

void Combat::lunar_actor(EntityState& actor) {
    auto& w = actor.words;
    const auto behavior = static_cast<Behavior>(w[26]);
    if (behavior == Behavior::drone || behavior == Behavior::grav_tank || behavior == Behavior::slug_drone) {
        const bool grav = behavior == Behavior::grav_tank, slug = behavior == Behavior::slug_drone;
        const auto kind = grav ? NodeKind::land : NodeKind::air;
        if (slug) { search(actor,40); if (w[5] > 0) { --w[5]; } }
        if (w[16] == -1) { nearest_node(actor,kind); }
        if (w[1] == -1) { ++nodes_.at(w[16])[static_cast<std::size_t>(kind)]; return; }
        if (slug) {
            scan_player(actor,player_scan_radius);
            face(actor,4);
            const auto& node = nodes_.at(w[16]);
            const int dx = node[4] - shift(w[7],8), dz = node[5] - shift(w[9],8);
            state_.node_range = dx * dx + dz * dz;
            const auto direction = normalize({{dx,0,dz}});
            w[10] = shift(direction[0] * w[3],4); w[12] = shift(direction[2] * w[3],4);
            const bool pursuing = w[22] != actor.slot;
            balance_height(actor,pursuing ? 60 + shift(sine(w[6] * 16),3) : 55);
            if (pursuing) { fire(actor); }
            if (state_.node_range < 2000) { change_node(actor,kind); }
        } else {
            search(actor,40,2);
            if (grav) { balance_height(actor,25); }
            w[10] = shift(w[10] * 31,5); w[12] = shift(w[12] * 31,5);
            const auto thrust = drone_thrust(w[13]);
            const int mx = shift(thrust[0] * w[3],8), mz = shift(thrust[1] * w[3],8);
            if (w[22] == actor.slot && !grav) {
                balance_height(actor,50);
                w[10] = shift(w[10] * 14,4); w[12] = shift(w[12] * 14,4);
            } else {
                if (w[22] != actor.slot) {
                    if (!grav) { balance_height(actor,150); }
                    follow_node(actor,10);
                }
                // Idle grav tanks consume the shared range left by the preceding callback.
                if (state_.node_range < 1000) { change_node(actor,kind); }
                else { w[10] += mx; w[12] += mz; }
            }
        }
        if (w[2] < 1) {
            ++nodes_.at(w[16])[static_cast<std::size_t>(kind)]; w[1] = 0;
            if (slug) { effect(actor,Behavior::bang,39); debris(actor); award(actor,59); }
            else { award(actor,59); effect(actor,Behavior::explosion,27); }
        }
        return;
    }
    if (behavior == Behavior::carryall) {
        if (w[5] > 0) { --w[5]; }
        if (w[20] == 10) {
            balance_height(actor,100); w[10] = sine(w[13]) * 8; w[12] = cosine(w[13]) * 8;
            if (w[6] >= 200) { w[5] = 60; w[20] = 11; }
        } else if (w[20] == 11) {
            balance_height(actor,40 + w[5]); w[13] += 4;
            if (w[5] == 0) { w[20] = 12; }
            w[10] = shift(w[10] * 31,5); w[12] = shift(w[12] * 31,5);
        } else if (w[20] == 12) {
            balance_height(actor,50 - w[5]); w[1] |= 0x20;
            if (w[14] < 30) { ++w[14]; }
            w[10] = sine(w[14]) * 16; w[12] = cosine(w[14]) * 16; --w[13];
        }
        if (w[2] < 1) {
            if (w[16] == -1) { ++state_.unassigned_air_returns; }
            else { load_nodes(); ++nodes_.at(w[16])[7]; }
            w[1] = 0; effect(actor,Behavior::bang,39); debris(actor); award(actor,1742);
        }
        return;
    }
    auto& parent = entity(w[23]).words;
    if (behavior == Behavior::hover_turret_base) {
        if (parent[20] == 10 || parent[20] == 11) {
            w[20] = 1; w[13] += 2;
            std::copy(parent.begin() + 7,parent.begin() + 10,w.begin() + 7); w[8] -= 9000;
            if (parent[2] < 1) { parent[20] = 0; }
        } else {
            w[20] = 0; w[1] = 0x801;
            search(actor,40); w[10] = shift(w[10] * 31,5); w[12] = shift(w[12] * 31,5);
            balance_height(actor,w[22] == actor.slot ? 40 : 80);
            if (w[2] < 1) { w[1] = 0; award(actor,59); effect(actor,Behavior::explosion,27); }
        }
        return;
    }
    const bool hovering = behavior == Behavior::hover_turret_top;
    if (hovering && parent[20] == 1) {
        w[13] = entity(parent[23]).words[13]; w[14] = w[15] = 0;
        std::copy(parent.begin() + 7,parent.begin() + 10,w.begin() + 7); w[8] += 4000;
        return;
    }
    std::copy(parent.begin() + 7,parent.begin() + 10,w.begin() + 7);
    if (hovering) { w[8] += 7000; w[1] = 0x1001; }
    if (w[5] > 0) { --w[5]; }
    w[22] = parent[22];
    if (w[22] == w[23]) {
        if (hovering) {
            if (w[14] < -60) { --w[14]; }
            w[13] = (w[13] + 3) & 511;
        } else {
            if (parent[13] < w[13]) { --w[13]; }
            if (w[13] < parent[13]) { ++w[13]; }
        }
    } else if (hovering) {
        face(actor,3); w[14] = std::clamp(w[14],-60,60); fire(actor);
    } else { aim(actor); }
    if (w[2] < 1) {
        w[1] = 0;
        if (behavior == Behavior::drone_gun) { parent[2] = 0; }
        if (hovering) { award(actor,59); effect(actor,Behavior::explosion,27); }
    }
    if (w[23] == actor.slot) {
        w[11] -= 30; w[13] += 25;
        if (w[8] <= height_(w[7],w[9])) {
            w[1] = 0;
            if (hovering) { effect(actor,Behavior::explosion,27); }
        }
    } else if (parent[2] < 1) {
        w[11] = behavior == Behavior::drone_gun ? 0 : 1200;
        w[23] = actor.slot; w[1] |= hovering ? 0x4012 : 0x12;
        if (hovering) { award(actor,111); }
    }
    if (!hovering) {
        auto& base = entity(w[23]).words;
        w[13] &= 511; base[13] &= 511;
        align_attitude(entity(w[23]),actor);
    }
}

}
