#include "combat.hpp"
#include "combat_math.hpp"

#include <stdexcept>

namespace swiv3d::detail {
namespace {
using namespace combat_math;
constexpr int helicopter_model = 990, ground_model = 991, wheel_model = 980, ski_model = 983;
constexpr int station_radius_squared = 15000, station_scan_radius_squared = 6000000;
constexpr int helicopter_engine_sound = 5000, ground_engine_sound = 5001, hover_engine_sound = 5002;
int engine_sound(CraftKind craft) {
    return craft == CraftKind::helicopter ? helicopter_engine_sound : craft == CraftKind::hover ? hover_engine_sound : ground_engine_sound;
}
constexpr std::array<std::array<int, 3>, 11> wheel_positions{{
    {{-4800,-1500,3000}}, {{4800,-1500,3000}}, {{-4800,-1500,-3000}}, {{4800,-1500,-3000}},
    {{-4800,-1500,-3000}}, {{4800,-1500,-3000}}, {{0,-1500,6000}},
    {{-5000,-1500,6000}}, {{5000,-1500,6000}}, {{-6200,-1500,-4000}}, {{6200,-1500,-4000}}
}};
}

EntityState& Combat::clone(EntityState& actor, Behavior behavior) {
    const auto words = actor.words;
    auto& result = spawn(actor, words[1], words[2], words[19], behavior);
    const auto next = result.words[0];
    result.words = words;
    result.words[0] = next;
    result.words[21] = 0;
    result.words[23] = actor.slot;
    result.words[26] = static_cast<int>(behavior);
    return result;
}

Combat::GroundContact Combat::ground_contact(EntityState& actor, int force) {
    const auto& w = actor.words;
    GroundContact contact{};
    const auto textures = (state_.water_flags & 2) != 0 ? resource_(102) : std::vector<std::uint8_t>{};
    const int floor = word(resource_(10000 + w[19]), 16) * 256;
    const int sp = sine(-w[14]), cp = cosine(-w[14]), sr = sine(-w[15]), cr = cosine(-w[15]);
    const int sy = sine(-w[13]), cy = cosine(-w[13]);
    for (int i = 0; i < 4; ++i) {
        const int x = (i & 1) == 0 ? -5120 : 5120, z = i < 2 ? 7680 : -7680;
        const int py = shift(floor * cp - z * sp, 8), pz = shift(floor * sp + z * cp, 8);
        const int rx = shift(x * cr - py * sr, 8), y = shift(x * sr + py * cr, 8);
        const int wx = shift(rx * cy - pz * sy, 8) + w[7], wz = shift(rx * sy + pz * cy, 8) + w[9];
        contact.texture = textures.empty() ? 255 : textures.at((shift(wz, 14) & 511) * 512 + (shift(wx, 14) & 511));
        contact.wheels[i] = w[8] + y - surface_(wx, wz);
        const auto behavior = static_cast<Behavior>(w[26]);
        if (contact.texture < 4 && (behavior == Behavior::ground_buggy || (behavior == Behavior::ground_ski && i >= 2))) {
            contact.wheels[i] += 2000;
        }
    }
    contact.momentum = rotate(actor, {{0,0,force}});
    const int lateral = shift(-shift(cp * 350, 8) * sr, 10), forward = shift(sp * 350, 8);
    contact.slide_x = cy * lateral - forward * sy;
    contact.slide_z = sy * lateral + forward * cy;
    return contact;
}

void Combat::settle(EntityState& actor, const GroundContact& contact) {
    auto& w = actor.words;
    const auto& wheels = contact.wheels;
    const int lowest = std::min(1000, *std::min_element(wheels.begin(), wheels.end()));
    for (int axis = 10; axis <= 12; ++axis) { w[axis] = shift(w[axis] * 14, 4); }
    if (lowest < 1000) {
        w[14] += shift(wheels[2] + wheels[3] - wheels[0] - wheels[1], 9);
        w[15] += shift(wheels[1] + wheels[3] - wheels[2] - wheels[0], 9);
    } else { w[14] -= 2; }
    if (lowest > 0) { w[11] -= 150; }
    if (lowest < 0) { w[11] = std::max(w[11], 0); w[8] -= lowest; }
}

void Combat::stop_engine() {
    state_.sound_events.push_back({state_.tick, state_.player, engine_sound(state_.flight.craft), SoundAction::stop});
}

void Combat::setup_vehicle(CraftKind craft, VehicleSetup setup) {
    auto& player = entity(state_.player);
    auto& w = player.words;
    auto& flight = state_.flight;
    if (setup == VehicleSetup::changeover && (flight.craft == CraftKind::buggy || flight.craft == CraftKind::ski)) {
        const int count = flight.craft == CraftKind::buggy ? 4 : 3;
        for (int i = 0; i < count; ++i) {
            const auto slot = state_.weapons.mounts[3 + i];
            if (slot != no_entity) { entity(slot).words[1] = 0; }
        }
    }
    if (setup == VehicleSetup::changeover) {
        stop_engine();
    }
    flight.craft = craft;
    flight.control = PlayerControl::normal;
    flight.weapon_hold = false;
    w[23] = player.slot;
    if (craft == CraftKind::helicopter || craft == CraftKind::hover) {
        w[19] = helicopter_model; w[1] = 0x217;
        w[26] = static_cast<int>(craft == CraftKind::hover ? Behavior::hover : Behavior::helicopter);
        flight.attitude = {w[13] * 256, w[14] * 256, w[15] * 128};
        state_.sound_events.push_back({state_.tick, state_.player, engine_sound(craft), SoundAction::loop});
        return;
    }
    w[19] = ground_model; w[1] = 0x207;
    w[26] = static_cast<int>(craft == CraftKind::ski ? Behavior::ground_ski : Behavior::ground_buggy);
    const int count = craft == CraftKind::ski ? 3 : 4;
    const int first = craft == CraftKind::ski ? 4 : state_.craft_set < 2 ? 0 : 7;
    for (int i = 0; i < count; ++i) {
        auto& wheel = spawn(player, 0x40, 0, craft == CraftKind::ski && i == 2 ? ski_model : wheel_model, Behavior::wheel);
        wheel.words[20] = first + i;
        state_.weapons.mounts[3 + i] = wheel.slot;
    }
    flight.wheel_direction = flight.wheel_target = 0;
    flight.engine_pitch = 11000;
    state_.sound_events.push_back({state_.tick, state_.player, engine_sound(craft), SoundAction::loop});
}

void Combat::change_station(EntityState& actor) {
    auto& w = actor.words;
    const bool ski = static_cast<Behavior>(w[26]) == Behavior::ski_station;
    if (w[5] > 0) { --w[5]; }
    w[2] = 1000;
    if (w[20] == 0 || w[20] == 5) {
        if ((w[6] & 31) == 0) {
            scan_player(actor,station_scan_radius_squared);
        }
        if (w[22] == actor.slot) { return; }
        auto& player = entity(w[22]).words;
        const int dx = shift(w[7] - player[7], 8), dz = shift(w[9] - player[9], 8);
        state_.object_direction = {dx,dz};
        if (dx * dx + dz * dz >= station_radius_squared || (player[1] & 0x21c) != (w[20] == 0 ? 0x214 : 0x204)) { return; }
        player[23] = actor.slot; w[23] = w[22]; player[1] = 0x40;
        player[10] = player[11] = player[12] = 0;
        player[20] = 0; player[5] = 70;
        player[26] = static_cast<int>(w[20] == 0 ? Behavior::helicopter_change : ski ? Behavior::ski_change : Behavior::buggy_change);
        state_.flight.control = w[20] == 0 ? PlayerControl::disabled : PlayerControl::ground_transition;
        state_.flight.weapon_hold = true;
        w[20] = w[20] == 0 ? 1 : 6;
        return;
    }
    if (w[20] == 1) {
        if (entity(w[23]).words[20] == 2) { w[20] = 2; }
        return;
    }
    if (w[20] == 2) {
        auto& player = entity(w[23]);
        clone(player, Behavior::parked_helicopter).words[23] = actor.slot;
        std::copy(w.begin() + 7, w.begin() + 10, player.words.begin() + 7);
        w[20] = 3; w[5] = 40; player.words[20] = 0;
        setup_vehicle(ski ? CraftKind::ski : CraftKind::buggy);
        return;
    }
    if (w[20] == 3) {
        auto& player = entity(w[23]).words;
        player[10] = sine(w[13]) * 4; player[12] = cosine(w[13]) * 4; player[13] = w[13];
        if (w[5] == 0) { w[20] = 4; w[5] = 200; w[22] = w[23] = actor.slot; }
        return;
    }
    if (w[20] == 4) { if (w[5] == 0) { w[20] = 5; } return; }
    if (w[20] == 6) { if (entity(w[23]).words[20] == 32) { w[20] = 7; } return; }
    if (w[20] == 8) { setup_vehicle(CraftKind::helicopter); w[5] = 200; w[20] = 9; return; }
    if (w[20] == 9 && w[5] == 0) { w[20] = 0; }
}

void Combat::vehicle_actor(EntityState& actor) {
    auto& w = actor.words;
    const auto behavior = static_cast<Behavior>(w[26]);
    if (behavior == Behavior::buggy_exit) {
        if (w[16] == -1) { nearest_node(actor,NodeKind::land); }
        if (w[1] == -1) { ++nodes_.at(w[16])[8]; return; }
        settle(actor,ground_contact(actor,0));
        w[10] = shift(w[10] * 14,4); w[11] = shift(w[11] * 14,4);
        if (w[20] != 0) { return; }
        const auto& node = nodes_.at(w[16]);
        const int dx = node[4] - shift(w[7],8), dz = node[5] - shift(w[9],8);
        state_.node_range = dx * dx + dz * dz;
        int best = -999999999, selected = 0, heading = w[13];
        constexpr int turn = 4;
        for (int i = 0; i < 5; ++i) {
            const int yaw = w[13] + (i - 2) * turn;
            const int score = sine(yaw) * dx + cosine(yaw) * dz;
            if (score > best) { best = score; selected = i; heading = yaw; }
        }
        w[13] = heading;
        const int thrust = selected == 0 || selected == 4 ? -1 : selected == 2 ? 2 : 1;
        w[10] += sine(w[13]) * thrust; w[12] += cosine(w[13]) * thrust;
        w[22] = w[23];
        if (state_.node_range < 4000) {
            const int previous = w[16]; change_node(actor);
            if (w[16] == previous) { w[20] = 1; }
        }
        return;
    }
    if (behavior == Behavior::helicopter_change) {
        if (w[5] > 0) { --w[5]; }
        const auto& station = entity(w[23]).words;
        const int heading = station[13] + 320;
        state_.object_direction = {station[7] + sine(heading) * 32,station[9] + cosine(heading) * 32};
        steer_towards(actor, station[7], station[9], 4);
        if (w[20] == 0) {
            for (int axis = 7; axis <= 9; ++axis) { w[axis] -= w[axis + 3]; }
            move_aircraft(actor, 130, height_); --w[6];
            w[14] = w[15] = 0;
            w[7] += shift(station[7] + sine(heading) * 32 - w[7], 4);
            w[9] += shift(station[9] + cosine(heading) * 32 - w[9], 4);
            if (w[5] == 0) { w[20] = 1; w[10] = w[11] = w[12] = 0; }
        } else if (w[20] == 1) {
            w[8] -= 550;
            if (w[8] < station[8] + 9500) { w[20] = 2; }
        }
        return;
    }
    const bool changing = behavior == Behavior::buggy_change || behavior == Behavior::ski_change;
    const auto contact = ground_contact(actor, changing ? 0 : 1000);
    if (changing) {
        settle(actor, contact);
        const auto& station = entity(w[23]).words;
        if (w[20] == 0) {
            const int x = shift(w[7], 8) + shift(sine(w[13]), 3), z = shift(w[9], 8) + shift(cosine(w[13]), 3);
            const int sx = shift(station[7], 8), sz = shift(station[9], 8), dx = sine(station[13]), dz = cosine(station[13]);
            const auto distance = [&](int side) { const int a = x - sx - dx * side, b = z - sz - dz * side; return a * a + b * b; };
            w[20] = distance(-1) <= distance(1) ? 20 : 10;
            return;
        }
        if (w[20] == 10 || w[20] == 20) {
            const int side = w[20] == 10 ? 256 : -256;
            state_.object_direction = {station[7] + sine(station[13]) * side,station[9] + cosine(station[13]) * side};
            if (steer_towards(actor, state_.object_direction[0], state_.object_direction[1], 2)) { ++w[20]; }
            return;
        }
        if (w[20] == 30) { if (steer_towards(actor, station[7], station[9], 3)) { w[20] = 31; } return; }
        if (w[20] == 11 || w[20] == 21 || w[20] == 31) {
            const int side = w[20] == 11 ? 256 : w[20] == 21 ? -256 : 0;
            if (w[20] == 31) { steer_towards(actor, station[7], station[9], 2); }
            w[7] += sine(w[13]) * 4; w[9] += cosine(w[13]) * 4;
            const int dx = shift(station[7] + sine(station[13]) * side - w[7], 8);
            const int dz = shift(station[9] + cosine(station[13]) * side - w[9], 8);
            if (dx * dx + dz * dz < (w[20] == 31 ? 5000 : 10000)) { w[20] = w[20] == 31 ? 32 : 30; }
        }
        return;
    }
    if (w[5] > 0) { --w[5]; }
    auto& flight = state_.flight;
    flight.power = std::clamp(flight.power, -500, 500);
    flight.steering_force = shift(flight.steering_force * 13, 4);
    flight.power = shift(flight.power * 15, 4);
    flight.steering_force = shift(flight.steering_force * 13, 4);
    const auto& wheels = contact.wheels;
    const int lowest = std::min(1000, *std::min_element(wheels.begin(), wheels.end()));
    if (lowest < 1000) {
        w[10] = shift(w[10] * (w[20] == 1 ? 14 : 15), 4);
        w[12] = shift(w[12] * (w[20] == 1 ? 14 : 15), 4);
        if (w[20] == 1) { flight.power = 0; }
        w[15] += shift(wheels[1] + wheels[3] - wheels[2] - wheels[0], 9);
        if (w[20] == 3) { w[20] = 0; }
        if (w[20] != 2) { w[14] += shift(wheels[2] + wheels[3] - wheels[0] - wheels[1], 9); }
        const int force_shift = contact.texture < 4 ? 13 : 11;
        if (contact.texture < 4 && w[2] > 0) { --w[2]; }
        if (w[20] != 2 && w[20] != 1) {
            if (contact.texture >= 4 || behavior == Behavior::ground_buggy) {
                w[10] += shift(contact.slide_x, contact.texture < 4 ? 10 : 8);
                w[12] += shift(contact.slide_z, contact.texture < 4 ? 10 : 8);
            }
        }
        for (int axis = 0; axis < 3; ++axis) { w[10 + axis] += shift(flight.power * contact.momentum[axis], force_shift); }
        const int speed = std::min(10, shift(std::int64_t(w[10]) * w[10] + std::int64_t(w[12]) * w[12], 18));
        flight.attitude[0] += speed * flight.steering_force;
    } else if (w[20] == 3) { if (w[14] > -50) { w[14] -= 4; } }
    else if (w[14] < 90) { w[14] += 2; }
    w[11] = shift(w[11] * 15, 4);
    if (lowest > 0) { w[11] -= 150; }
    if (lowest < 0) {
        if (w[11] < -1100) { sound(actor, 4011); }
        w[11] = std::max(w[11], 0); w[8] -= lowest;
    }
    const auto square = static_cast<std::uint32_t>(w[10]) * w[10] + static_cast<std::uint32_t>(w[11]) * w[11] + static_cast<std::uint32_t>(w[12]) * w[12];
    state_.object_direction[0] = 7000 + shift(static_cast<std::int32_t>(square), 10) - flight.engine_pitch;
    flight.engine_pitch += std::min(100, state_.object_direction[0]);
    if (flight.engine_pitch > 20500) { flight.engine_pitch = 15000; }
}

void Combat::place_wheels(int steering_step) {
    auto& flight = state_.flight;
    auto& player = entity(state_.player);
    if (flight.wheel_direction < flight.wheel_target) { flight.wheel_direction += steering_step; }
    if (flight.wheel_target < flight.wheel_direction) { flight.wheel_direction -= steering_step; }
    const int count = flight.craft == CraftKind::ski ? 3 : 4;
    const auto textures = (state_.water_flags & 2) != 0 ? resource_(102) : std::vector<std::uint8_t>{};
    for (int i = 0; i < count; ++i) {
        auto& wheel = entity(state_.weapons.mounts[3 + i]);
        auto& w = wheel.words;
        std::copy(player.words.begin() + 10, player.words.begin() + 13, w.begin() + 10);
        const auto& position = wheel_positions.at(w[20]);
        place(player, wheel, position[0], position[1], position[2]);
        const int ground = surface_(w[7], w[9]);
        if (w[8] <= ground + 4000) { w[8] = ground + 2000; }
        const int texture = textures.empty() ? 255 : textures.at((shift(w[9], 14) & 511) * 512 + (shift(w[7], 14) & 511));
        w[13] = player.words[13]; w[15] = player.words[15];
        if (flight.craft == CraftKind::ski && i == 2) {
            w[19] = ski_model; w[14] = player.words[14]; w[13] += flight.wheel_direction;
            continue;
        }
        const bool water = texture < 4 && w[8] - ground - 1500 < 2000;
        if (water) { w[19] = 982; w[14] = 0; w[8] = ground; }
        else {
            w[19] = 980;
            if (flight.craft == CraftKind::ski) { w[14] = player.words[14]; }
            else { w[14] -= shift(cosine(w[13]) * w[12] + sine(w[13]) * w[10], 13); }
            w[8] = w[8] > (w[13] & 511) + 1500 ? w[8] - 1500 : w[13] & 511;
        }
        if (flight.craft == CraftKind::ski) { continue; }
        w[13] += i < 2 || water ? flight.wheel_direction : -shift(flight.wheel_direction, 1);
    }
}

void Combat::vehicle_control(const KeyboardState& keys) {
    auto& flight = state_.flight;
    if (flight.control == PlayerControl::disabled) { return; }
    if (flight.control == PlayerControl::ground_transition) { place_wheels(8); return; }
    auto& player = entity(state_.player);
    auto& w = player.words;
    ++flight.control_ticks;
    if (w[20] == 1) { w[20] = 0; }
    flight.wheel_target = 0;
    const bool analog_turn = flight.controller != ControllerKind::keyboard && flight.axis_bindings[0] != 0;
    const bool analog_move = flight.controller != ControllerKind::keyboard && flight.axis_bindings[1] != 0;
    if (analog_turn) {
        const int axis = flight.axes.at(flight.axis_bindings[0] - 1);
        flight.wheel_target = shift(axis * 100,8);
        w[13] += shift(axis * 10,8);
    } else {
        if (keys[flight.bindings[0]]) { flight.wheel_target = -40; w[13] -= 4; }
        if (keys[flight.bindings[1]]) { flight.wheel_target = 40; w[13] += 4; }
    }
    const int acceleration = (keys[flight.bindings[2]] ? 130 : 0) - (keys[flight.bindings[3]] ? 130 : 0);
    if (analog_move) {
        const int axis = flight.axes.at(flight.axis_bindings[1] - 1);
        flight.power += shift(axis * 260,8);
        if (flight.craft == CraftKind::buggy) { entity(state_.weapons.mounts[5]).words[14] -= shift(axis * 30,8); }
    } else {
        flight.power += acceleration;
        if (flight.craft == CraftKind::buggy) {
            for (int i : {5,6}) { entity(state_.weapons.mounts[i]).words[14] -= acceleration / 10; }
        }
    }
    if ((!analog_move && ((keys[flight.bindings[2]] && keys[flight.bindings[3]]) ||
         (flight.craft == CraftKind::buggy && keys[flight.bindings[4]]))) ||
        (keys[flight.bindings[10]] && (flight.craft != CraftKind::buggy || !analog_move || w[20] == 0))) { w[20] = 1; }
    if (w[20] != 1) {
        flight.power += acceleration;
        if (flight.craft == CraftKind::buggy) {
            for (int i : {5,6}) { entity(state_.weapons.mounts[i]).words[14] -= acceleration / 10; }
        }
    }
    if (!keys[flight.bindings[5]]) { flight.previous_heading = w[13]; }
    if (sine(w[13]) * w[10] + cosine(w[13]) * w[12] < -7000) { flight.wheel_target = -flight.wheel_target; }
    place_wheels(4);
}

void Combat::finish_vehicle() {
    auto& player = entity(state_.player).words;
    if (player[20] == 1) { player[20] = 0; }
    state_.flight.power += 130;
    player[13] -= 4;
    entity(state_.weapons.mounts[5]).words[14] -= 13;
    if (state_.flight.craft == CraftKind::buggy) { entity(state_.weapons.mounts[6]).words[14] -= 13; }
    place_wheels(8);
}

}
