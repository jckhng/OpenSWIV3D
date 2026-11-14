#include "combat.hpp"
#include "combat_math.hpp"

#include <stdexcept>

namespace swiv3d::detail {
namespace {
using namespace combat_math;
constexpr int intruder_slots_offset = 0x194;
constexpr int actor_record_bytes = 27 * sizeof(std::int32_t);
}

void Combat::ground_blast(EntityState& actor) {
    const int bound_size = bound(actor.words[19]);
    state_.object_direction[0] = bound_size;
    const int size = bound_size < 1000 ? 0 : bound_size < 5000 ? 1 : 2;
    if (deform_) { deform_(actor, size); }
    auto& explosion = add_object(actor, 0xe1, 3000, 900 + size, Behavior::ground_explosion);
    illuminate(actor, size);
    sound(explosion, 1000);
}

bool Combat::steer_towards(EntityState& actor, int x, int z, int turn) {
    auto& steps = state_.direction_steps;
    auto& w = actor.words;
    const int dx = shift(x - w[7], 8), dz = shift(z - w[9], 8);
    steps[1] = -turn;
    steps[3] = turn;
    const auto& direction = state_.object_direction;
    state_.node_range = static_cast<std::int32_t>(static_cast<std::uint32_t>(direction[0]) * direction[0] +
                                                static_cast<std::uint32_t>(direction[1]) * direction[1]);
    int best = -999999999, selected = 0;
    for (int i = 0; i < 5; ++i) {
        const int score = sine(w[13] + steps[i]) * dx + cosine(w[13] + steps[i]) * dz;
        if (score > best) { best = score; selected = i; }
    }
    steps[0] = -turn * 2; steps[4] = turn * 2;
    w[13] += steps[selected];
    return selected == 2;
}

void Combat::load_nodes() {
    if (!nodes_.empty()) { return; }
    const auto bytes = resource_(107);
    constexpr std::size_t node_bytes = 36;
    for (std::size_t offset = 0; offset + node_bytes <= bytes.size(); offset += node_bytes) {
        std::array<int, 9> node{};
        for (std::size_t i = 0; i < node.size(); ++i) { node[i] = word(bytes, offset + i * 4); }
        if (node[6] == -1) { break; }
        nodes_.push_back(node);
    }
}

void Combat::nearest_node(EntityState& actor, NodeKind kind) {
    load_nodes();
    auto& w = actor.words;
    const auto capacity = static_cast<std::size_t>(kind);
    if (w[16] != -1) { ++nodes_.at(w[16])[capacity]; }
    int best = 999999999;
    w[16] = -1;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        const auto& node = nodes_[i];
        if (node[capacity] <= 0) { continue; }
        const int dx = node[4] - shift(w[7], 8), dz = node[5] - shift(w[9], 8);
        const int distance = dx * dx + dz * dz;
        if (distance < best) { best = distance; w[16] = static_cast<int>(i); }
    }
    if (w[16] == -1) { throw std::runtime_error("No available mission node"); }
    --nodes_.at(w[16])[capacity];
}

void Combat::patrol(EntityState& actor) {
    auto& w = actor.words;
    if (w[16] == -1) { nearest_node(actor, NodeKind::air); }
    if (w[1] == -1) { ++nodes_.at(w[16])[7]; return; }
    if (w[5] > 0) { --w[5]; }
    search(actor, 40, 2);
    for (int i = 7; i <= 9; ++i) { w[i] -= w[i + 3]; }
    move_aircraft(actor, 80, height_);
    --w[6];
    const auto& node = nodes_.at(w[16]);
    const int dx = node[4] - shift(w[7], 8), dz = node[5] - shift(w[9], 8);
    const int distance = dx * dx + dz * dz;
    state_.node_range = distance;
    state_.object_direction = {dx, dz};
    const bool pursuing = w[22] != actor.slot;
    const int ground = std::max(height_(w[7], w[9]), height_(w[7] + w[10] * 32, w[9] + w[12] * 32));
    const int error = shift(ground, 8) + (pursuing ? 70 : 40) - shift(w[8], 8);
    w[11] = shift(w[11] * 14, 4);
    if (error > 0) { w[11] += error * 2; }
    if (error < 0) { w[11] += shift(error, 2); }
    int selected = 0, best = -999999999;
    for (int i = 0; i < 8; ++i) {
        const int score = sine(w[13] + i * 64) * dx + cosine(w[13] + i * 64) * dz;
        if (score > best) { best = score; selected = i; }
    }
    w[13] &= 511;
    constexpr int pitch[]{-4,-3,0,3,4,3,0,-3}, bank[]{0,5,6,5,0,-5,-6,-5}, yaw[]{0,3,4,4,4,-4,-4,-3};
    if ((pitch[selected] < 0 && w[14] > -25) || (pitch[selected] > 0 && w[14] < 25)) { w[14] += pitch[selected]; }
    if ((bank[selected] < 0 && w[15] > -25) || (bank[selected] > 0 && w[15] < 25)) { w[15] += bank[selected]; }
    if (pursuing || selected == 0) {
        const auto& target = entity(w[22]).words;
        const int tx = pursuing ? shift(target[7], 8) - shift(w[7], 8) : dx;
        const int tz = pursuing ? shift(target[9], 8) - shift(w[9], 8) : dz;
        const int turn = pursuing ? 8 : 2;
        int heading = w[13];
        best = -999999999;
        for (int offset : {-turn,0,turn}) {
            const int score = sine(w[13] + offset) * tx + cosine(w[13] + offset) * tz;
            if (score > best) { best = score; heading = w[13] + offset; }
        }
        w[13] = heading;
    } else { w[13] += yaw[selected]; }
    if (pursuing) { fire(actor); }
    if (distance < 2000) { change_node(actor, NodeKind::air); }
    if (w[2] < 1) {
        w[26] = static_cast<int>(Behavior::dead_aircraft);
        effect(actor, Behavior::smoker, 55);
        w[6] = 0;
        ++nodes_.at(w[16])[7];
        w[1] |= 0x4020;
        award(actor, 210);
        effect(actor, Behavior::aircraft_explosion, 13);
    }
}

void Combat::mission_actor(EntityState& actor) {
    auto& w = actor.words;
    const auto behavior = static_cast<Behavior>(w[26]);
    if (behavior == Behavior::ground_exit) {
        if (w[5] > 0) { --w[5]; }
        w[2] = 1000;
        if (w[20] == 5) {
            if ((w[6] & 31) == 0) { scan_player(actor,6000000); }
            if (w[22] == actor.slot) { return; }
            auto& player = entity(w[22]).words;
            const int dx = shift(w[7] - player[7],8), dz = shift(w[9] - player[9],8);
            if (dx * dx + dz * dz >= 900000 || (player[1] & 0x20c) != 0x204) { return; }
            check_objective(actor,ObjectiveFinish::ground_exit);
            player[23] = actor.slot; w[23] = w[22];
            state_.flight.control = PlayerControl::ground_transition;
            state_.flight.weapon_hold = true;
            player[1] = 0x40; player[10] = player[11] = player[12] = 0;
            player[26] = static_cast<int>(Behavior::buggy_exit); player[20] = 0; player[5] = 70;
            w[20] = 6;
        } else if (w[20] == 6) {
            if (entity(w[23]).words[20] == 32) { w[20] = 7; }
        } else if (w[20] == 8) {
            setup_vehicle(CraftKind::helicopter); w[5] = 200; w[20] = 9;
        } else if (w[20] == 9 && w[5] == 0) { w[20] = 0; }
        return;
    }
    if (behavior == Behavior::parked_helicopter) {
        auto& station = entity(w[23]).words;
        if (station[20] == 7) {
            auto& player = entity(station[23]).words;
            std::copy(w.begin() + 7, w.begin() + 10, player.begin() + 7);
            std::copy(w.begin() + 13, w.begin() + 16, player.begin() + 13);
            station[20] = 8; w[1] = 0;
        }
        return;
    }
    if (behavior == Behavior::change_station || behavior == Behavior::ski_station) {
        change_station(actor);
        return;
    }
    if (behavior == Behavior::air_patrol) { patrol(actor); return; }
    if (behavior == Behavior::submarine) {
        if (!model_bounds_) { throw std::runtime_error("Submarine needs mutable model bounds"); }
        if (w[5] > 0) { --w[5]; }
        model_bounds_(w[19],2000);
        search(actor,40,2);
        if (w[22] != actor.slot) { fire(actor); }
        if (w[20] == 0 || w[20] == 1) {
            w[10] = w[20] == 0 ? shift(sine(w[13]) * w[3],5) : 0;
            w[12] = w[20] == 0 ? shift(cosine(w[13]) * w[3],5) : 0;
            w[11] = 0; w[8] = surface_(w[7],w[9]) + 4000;
            const auto textures = resource_(102);
            if (textures.at((shift(w[9],14) & 511) * 512 + (shift(w[7],14) & 511)) != 0) { w[20] = 1; }
        }
        if (w[2] < 1) {
            w[1] = 0; award(actor,59);
            model_bounds_(w[19],6000);
            effect(actor,Behavior::explosion,27);
            debris(actor);
            model_bounds_(w[19],1600);
        }
        return;
    }
    if (behavior == Behavior::intruder) {
        if (w[5] > 0) { --w[5]; }
        w[22] = entity(w[23]).words[22];
        for (int axis = 7; axis <= 9; ++axis) { w[axis] -= w[axis + 3]; }
        move_aircraft(actor,80,height_); --w[6];
        int x = 0, z = 0;
        if (w[22] != actor.slot) {
            fire(actor);
            const auto& target = entity(w[22]).words;
            x = shift(target[7],8); z = shift(target[9],8);
            if (w[20] == 0) { x += 700; }
            if (w[20] == 1) { z += 700; }
            if (w[20] == 2) { x -= 700; }
            if (w[20] == 3) { z -= 700; }
            if (target[2] < 1) { w[22] = actor.slot; }
        }
        const int ground = std::max(height_(w[7],w[9]),height_(w[7] + w[10] * 32,w[9] + w[12] * 32));
        const int error = shift(ground,8) + 70 - shift(w[8],8);
        w[11] = shift(w[11] * 14,4);
        if (error > 0) { w[11] += error * 2; }
        if (error < 0) { w[11] += shift(error,2); }
        int best = -999999999, selected = 0;
        for (int i = 0; i < 8; ++i) {
            const int score = sine(w[13] + i * 64) * (x - shift(w[7],8)) + cosine(w[13] + i * 64) * (z - shift(w[9],8));
            if (score > best) { best = score; selected = i; }
        }
        w[13] &= 511;
        constexpr int pitch[]{-4,-3,0,3,4,3,0,-3}, bank[]{0,5,6,5,0,-5,-6,-5};
        if ((pitch[selected] < 0 && w[14] > -25) || (pitch[selected] > 0 && w[14] < 25)) { w[14] += pitch[selected]; }
        if ((bank[selected] < 0 && w[15] > -25) || (bank[selected] > 0 && w[15] < 25)) { w[15] += bank[selected]; }
        const auto& target = entity(w[22]).words;
        int heading = w[13]; best = -999999999;
        for (int offset : {-8,0,8}) {
            const int score = sine(w[13] + offset) * (shift(target[7],8) - shift(w[7],8)) +
                              cosine(w[13] + offset) * (shift(target[9],8) - shift(w[9],8));
            if (score > best) { best = score; heading = w[13] + offset; }
        }
        w[13] = heading;
        if (w[2] < 1 || w[1] == 0) {
            // The original node -1 air counter aliases the word before the array.
            if (w[16] == -1) { ++state_.unassigned_air_returns; }
            else { load_nodes(); ++nodes_.at(w[16])[7]; }
            w[26] = static_cast<int>(Behavior::dead_aircraft); w[6] = 0; w[1] |= 0x4020;
            effect(actor,Behavior::smoker,55); award(actor,210);
            effect(actor,Behavior::aircraft_explosion,13);
            if (w[22] != actor.slot) {
                if (w[23] == state_.player) { state_.weapons.mounts[7 + w[20]] = no_entity; }
                else {
                    // The original clears its parent's extended player slot even
                    // when that parent is a pooled factory: this aliases a later actor.
                    const int offset = intruder_slots_offset + w[20] * sizeof(std::int32_t);
                    const int slot = w[23] + offset / actor_record_bytes;
                    const int field = offset % actor_record_bytes / sizeof(std::int32_t);
                    if (slot >= static_cast<int>(object_slots)) { throw std::runtime_error("Intruder slot alias exceeds the actor pool"); }
                    entity(slot).words[field] = field == 0 || field == 22 || field == 23 ? no_entity : 0;
                }
            }
        }
        return;
    }
    if (behavior == Behavior::apc_wheel) {
        const auto& parent = entity(w[23]).words;
        std::copy(parent.begin() + 10, parent.begin() + 13, w.begin() + 10);
        w[14] -= shift(cosine(w[13]) * w[12] + sine(w[13]) * w[10], 13);
        if (w[20] >= 0 && w[20] < 4) {
            place(entity(w[23]), actor, (w[20] & 1) == 0 ? -6000 : 6000, -4000, w[20] < 2 ? 8000 : -4000);
        }
        w[13] = parent[13];
        if (parent[1] == 0) { w[1] = 0; }
        return;
    }
    if (behavior == Behavior::radar) {
        auto& parent = entity(w[23]).words;
        std::copy(parent.begin() + 7, parent.begin() + 10, w.begin() + 7);
        if (w[20] == 1) { w[13] = parent[13] + shift(sine((w[6] & 63) * 8), 2); }
        if (w[20] == 2) { w[13] = parent[13] + sine((w[6] & 127) * 4); }
        if (w[20] == 3) { w[13] += 5; }
        if (w[2] < 1 || parent[1] == 0) { w[1] = 0; parent[2] = 0; }
        return;
    }
    if (behavior == Behavior::pillbox) {
        if (w[5] > 0) { --w[5]; }
        search(actor, 40);
        w[1] |= 0x50000;
        const auto& target = entity(w[22]).words;
        const int dx = shift(target[7] - w[7], 8), dz = shift(target[9] - w[9], 8);
        if (w[22] != actor.slot && shift(dx * dx + dz * dz, 8) < 1600) { fire(actor); }
        else { w[5] = 41; }
        if (w[2] < 1) { w[1] = 0; effect(actor, Behavior::explosion, 27); }
        return;
    }
    if (behavior == Behavior::attack_plane || behavior == Behavior::strafe_plane) {
        if (w[5] > 0) { --w[5]; }
        w[10] = shift(sine(w[13]) * w[3], 2);
        w[12] = shift(cosine(w[13]) * w[3], 2);
        const auto& target = entity(w[22]).words;
        const int dx = shift(target[7], 8) - shift(w[7], 8), dz = shift(target[9], 8) - shift(w[9], 8);
        if (behavior == Behavior::attack_plane && w[20] == 0) {
            int best = -999999999, heading = w[13];
            for (int offset : {-1,0,1}) {
                const int score = sine(w[13] + offset) * dx + cosine(w[13] + offset) * dz;
                if (score > best) { best = score; heading = w[13] + offset; }
            }
            w[13] = heading;
        }
        if (behavior == Behavior::attack_plane) {
            if (dx * dx + dz * dz < 2000) { w[20] = 1; }
            else if (w[20] == 1) { w[20] = 2; }
        }
        if (behavior == Behavior::strafe_plane || w[20] == 0) { fire(actor); }
        const int ground = std::max(height_(w[7], w[9]), height_(w[7] + w[10] * 32, w[9] + w[12] * 32));
        const int error = shift(ground, 8) + 190 - shift(w[8], 8);
        if (error > 0) { w[11] += error * 2; }
        if (error < 0) { w[11] += shift(error, 1); }
        const int drag = shift(w[11], 4);
        w[11] -= drag == 0 ? w[11] : drag;
        if (w[2] < 1) { w[1] = 0; effect(actor, Behavior::explosion, 27); award(actor, 50); }
        w[14] = shift(w[11], 5);
        return;
    }
    const auto bytes = resource_(105);
    const auto field = [&](int index) { return word(bytes, w[25] + index * 4); };
    if (behavior == Behavior::conditional_pickup || behavior == Behavior::completion_pickup) {
        const bool completion = behavior == Behavior::completion_pickup;
        if (w[20] == 0 && (completion ? state_.objective_state == 1 : state_.last_objective == 9)) {
            auto& pickup = add_object(actor,0x41,field(6),field(5),Behavior::pickup);
            pickup.words[8] = height_(pickup.words[7],pickup.words[9]);
            pickup.words[13] = field(3); pickup.words[21] = field(7); w[20] = 1;
        }
        return;
    }
    if (behavior == Behavior::intruder_factory) {
        ++state_.game_counter;
        const auto& player = entity(state_.player).words;
        const int dx = shift(w[7] - player[7],8), dz = shift(w[9] - player[9],8);
        w[22] = (player[1] & 0x40) == 0 && dx * dx + dz * dz < 6000000 ? state_.player : actor.slot;
        if (w[22] != actor.slot && w[6] > 30) {
            for (int index = 0; index < 4; ++index) {
                if (state_.weapons.mounts[7 + index] != no_entity) { continue; }
                auto& helicopter = spawn(actor,0x1033,field(6),field(4),Behavior::intruder);
                helicopter.words[7] = player[7] - sine(w[13]) * 1500;
                helicopter.words[9] = player[9] - cosine(w[13]) * 1500;
                helicopter.words[8] += 99999; helicopter.words[3] = field(8);
                helicopter.words[21] = field(7); helicopter.words[20] = index; helicopter.words[22] = w[22];
                state_.weapons.mounts[7 + index] = helicopter.slot;
                break;
            }
            w[6] = 0;
        }
        return;
    }
    if (behavior == Behavior::tank_factory) {
        if (w[16] == -1) { nearest_node(actor, NodeKind::land); ++nodes_.at(w[16])[8]; }
        if (w[6] > 140 && nodes_.at(w[16])[8] > 0) {
            auto& base = spawn(actor, 0x1023, field(13), field(11), Behavior::tank);
            base.words[3] = field(15);
            base.words[22] = base.slot;
            auto& top = spawn(base, 0x1023, field(13), field(12), Behavior::tank_top);
            top.words[21] = field(14);
            w[6] = 0;
        }
    } else if (behavior == Behavior::burst_factory) {
        search(actor,30);
        if (w[5] > 0) { --w[5]; }
        if (w[22] != actor.slot && w[5] < 48 && (w[5] & 7) == 0) {
            auto& plane = spawn(actor,0x1033,field(13),field(11),Behavior::strafe_plane);
            plane.words[13] += w[5] * 2 - 50;
            const auto& target = entity(w[22]).words;
            plane.words[7] = target[7] - sine(w[13]) * 1800 + sine(plane.words[13]) * 256;
            plane.words[9] = target[9] - cosine(w[13]) * 1800 + cosine(plane.words[13]) * 256;
            plane.words[8] += 99999; plane.words[3] = field(15);
            plane.words[22] = w[22]; plane.words[21] = field(14);
        }
        if (w[5] == 0) { w[5] = 160; }
    } else if (behavior == Behavior::plane_factory || behavior == Behavior::strafe_factory) {
        search(actor, 30);
        const int interval = behavior == Behavior::strafe_factory ? 90 : 100;
        if (w[22] != actor.slot && w[6] > interval) {
            auto& plane = spawn(actor, 0x1033, field(13), field(11),
                                behavior == Behavior::strafe_factory ? Behavior::strafe_plane : Behavior::attack_plane);
            const auto& target = entity(w[22]).words;
            plane.words[7] = target[7] - sine(w[13]) * 1500;
            plane.words[9] = target[9] - cosine(w[13]) * 1500;
            plane.words[8] += 99999;
            plane.words[3] = field(15);
            plane.words[22] = w[22];
            plane.words[21] = field(14);
            w[6] = 0;
        }
    }
    if (w[2] < 1) {
        w[1] = 0;
        ground_blast(actor);
        debris(actor);
        award(actor, 500);
        check_objective(actor);
    }
}

}
