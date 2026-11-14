#include "combat.hpp"
#include "combat_jitter.hpp"
#include "combat_math.hpp"
#include "game_text.hpp"
#include "ground_effects.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace swiv3d::detail {
namespace {

using namespace combat_math;
constexpr int frozen = 0x80000;
constexpr int world_mask = 0x7fffff;
constexpr int effect_model = 10000;
constexpr int activity_side = 11;
constexpr std::array<int, activity_side * activity_side> activity{{
    2,2,2,1,1,1,1,1,2,2,2, 2,2,1,1,0,0,0,1,1,2,2,
    2,1,1,0,0,0,0,0,1,1,2, 1,1,0,0,0,0,0,0,0,1,1,
    1,0,0,0,0,0,0,0,0,0,1, 1,0,0,0,0,0,0,0,0,0,1,
    1,0,0,0,0,0,0,0,0,0,1, 1,1,0,0,0,0,0,0,0,1,1,
    2,1,1,0,0,0,0,0,1,1,2, 2,2,1,1,0,0,0,1,1,2,2,
    2,2,2,1,1,1,1,1,2,2,2
}};
constexpr std::array<std::array<int, 3>, 5> aim_vectors{{
    {{0,0,1000}}, {{-44,0,999}}, {{44,0,999}}, {{0,44,999}}, {{0,-44,999}}
}};

std::size_t cell(const EntityState& actor) { return (shift(actor.words[7], 17) & 63) + (shift(actor.words[9], 11) & 4032); }
}

Combat::Combat(MissionState& state, Height height, Resource resource, Deform deform, Height surface,
               std::optional<CraftKind> initial_craft, ModelBounds bounds)
    : state_(state), height_(std::move(height)), surface_(surface ? std::move(surface) : height_),
      resource_(std::move(resource)), deform_(std::move(deform)), model_bounds_(std::move(bounds)) {
    indices_.fill(-1);
    state_.entities.reserve(object_slots + 1);
    for (std::size_t i = 0; i < state_.entities.size(); ++i) {
        indices_.at(state_.entities[i].slot) = static_cast<int>(i);
    }
    for (int slot = state_.free_slot; slot >= 0; --slot) {
        free_.push_back(slot);
    }
    if (initial_craft) { setup_vehicle(*initial_craft,VehicleSetup::initial); }
}

EntityState& Combat::entity(int slot) { return state_.entities.at(indices_.at(slot)); }

void Combat::apply_cheat(int action) {
    if (action < 0) { return; }
    if (action < 200) { give(action); return; }
    switch (action) {
    case 200: setup_vehicle(CraftKind::helicopter); break;
    case 201: setup_vehicle(CraftKind::buggy); break;
    case 202: setup_vehicle(CraftKind::ski); break;
    case 203: setup_vehicle(CraftKind::hover); break;
    default: throw std::runtime_error("Unsupported cheat craft");
    }
}

void Combat::link(EntityState& actor) {
    auto& head = state_.grid[cell(actor)];
    actor.words[0] = head;
    head = actor.slot;
}

EntityState& Combat::spawn(EntityState& parent, int flags, int health, int model, Behavior behavior) {
    if (free_.empty()) {
        throw std::runtime_error("Mission object pool exhausted");
    }
    const int slot = free_.front();
    free_.pop_front();
    state_.free_slot = free_.empty() ? no_entity : free_.front();
    const auto source = parent.words;
    if (indices_[slot] == -1) {
        indices_[slot] = static_cast<int>(state_.entities.size());
        state_.entities.push_back({slot, {}});
    }
    auto& result = entity(slot);
    result.words = source;
    auto& w = result.words;
    w[1] = flags; w[2] = health; w[6] = 0; w[16] = -1;
    w[19] = model; w[20] = 0; w[21] = 0; w[23] = parent.slot;
    w[26] = static_cast<int>(behavior);
    link(result);
    return result;
}

void Combat::retire(EntityState& actor) {
    actor.words[0] = no_entity;
    free_.push_back(actor.slot);
    state_.free_slot = free_.front();
}

int Combat::bound(int model) {
    return model == 0 || model >= effect_model ? 100 : word(resource_(10000 + model), 12);
}

void Combat::effect(EntityState& actor, Behavior behavior, int frame) {
    const int model = behavior == Behavior::aircraft_explosion ? 11002 : behavior == Behavior::smoker || behavior == Behavior::smoke ? 11004 :
                      behavior == Behavior::explosion ? 11003 : 11000;
    auto& result = spawn(actor, 0xe1, 100, model, behavior);
    result.words[10] = result.words[11] = result.words[12] = 0;
    result.words[20] = frame;
    if (behavior == Behavior::explosion) { sound(result, 1001); }
    if (behavior == Behavior::aircraft_explosion) { sound(result, 1003); }
}

void Combat::smoke(EntityState& actor) {
    auto& result = spawn(actor, 0xe1, 100, 11004, Behavior::smoke);
    const auto& vector = combat_jitter[state_.effects_phase];
    result.words[10] = shift(vector[0], 4);
    result.words[11] = vector[1] + 500;
    result.words[12] = shift(vector[2], 4);
    result.words[20] = 46;
    state_.effects_phase = (state_.effects_phase + 1) & 255;
}

void Combat::track_gun(const EntityState& player) {
    auto& pitch = state_.gun_pitch;
    if (player.words[22] == player.slot) {
        if (pitch > 0) { pitch -= 3; }
        if (pitch < 0) { pitch += 3; }
    } else {
        constexpr int offsets[]{-15,-8,-3,-1,0,1,3,8,15};
        const auto& target = entity(player.words[22]).words;
        state_.object_direction = {target[7] - player.words[7], target[9] - player.words[9]};
        int best = -999999999, selected = 0;
        auto gun = player;
        for (int offset : offsets) {
            gun.words[14] = pitch + offset;
            const auto direction = rotate(gun, {{0,0,100}});
            std::int64_t product = 0;
            for (int axis = 0; axis < 3; ++axis) { product += static_cast<std::int64_t>(direction[axis]) * (target[7+axis] - player.words[7+axis]); }
            const auto score = static_cast<std::int32_t>(static_cast<std::uint32_t>(product));
            if (score > best) { best = score; selected = offset; }
        }
        pitch += selected;
    }
    if (pitch > 60) { pitch = 25; }
    if (pitch < -60) { pitch = -25; }
}

void Combat::illuminate(const EntityState& actor, int size) {
    constexpr int light_side = 64, light_scale = 64;
    const int outer_light = (size + 2) * light_scale, adjacent_light = outer_light * 2, center_light = outer_light * 4;
    const int x = shift(actor.words[7], 14), z = shift(actor.words[9], 14);
    for (int dz = -2; dz <= 2; ++dz) {
        for (int dx = -2; dx <= 2; ++dx) {
            if (std::abs(dx) + std::abs(dz) > 2) { continue; }
            const int light = dx == 0 && dz == 0 ? center_light : std::abs(dx) + std::abs(dz) == 1 ? adjacent_light : outer_light;
            state_.lighting[((z + dz) & (light_side - 1)) * light_side + ((x + dx) & (light_side - 1))] += light;
        }
    }
}

void Combat::destroy(EntityState& actor) {
    const bool scenery = static_cast<Behavior>(actor.words[26]) == Behavior::pose;
    actor.words[1] = 0;
    if (scenery) {
        ground_blast(actor);
        debris(actor);
        award(actor, 73);
        reveal_pickup(actor);
    } else { effect(actor, Behavior::explosion, 27); }
    ++state_.kills;
    check_objective(actor);
}

void Combat::check_objective(EntityState& actor, ObjectiveFinish finish) {
    const auto found = std::find_if(state_.objective_targets.begin(), state_.objective_targets.end(),
        [&](const auto& objective) { return objective.entity == actor.slot; });
    if (found == state_.objective_targets.end()) { return; }
    const int priority = found->priority;
    state_.objective_targets.erase(found);
    ++state_.objectives_completed;
    if (std::any_of(state_.objective_targets.begin(), state_.objective_targets.end(),
        [&](const auto& objective) { return objective.priority == priority; })) { return; }
    if (priority == 0 && state_.phase == MissionPhase::playing && state_.finish_counter == -1) {
        if (finish == ObjectiveFinish::extraction) { cue_message(state_,20,0,0x1400,MessageMotion::held); }
        state_.finish = finish == ObjectiveFinish::ground_exit ? FinishKind::delayed_success : FinishKind::success;
        state_.finish_counter = finish == ObjectiveFinish::ground_exit ? 80 : 0;
    }
    else if (priority != 0) { cue_message(state_,20,priority,0x1400); }
    if (priority == 9) { state_.last_objective = 9; }
    if (state_.objective_state == 0 && !state_.objective_targets.empty() && state_.objective_targets.front().priority == 0) {
        state_.objective_state = 1;
    }
}

void Combat::kill_player(EntityState& player) {
    cue_message(state_,20,10,0x1000,MessageMotion::held);
    auto& w = player.words;
    state_.flight.control = PlayerControl::disabled;
    stop_engine();
    w[1] = 0x40;
    effect(player, Behavior::explosion, 27);
    debris(player);
    effect(player, Behavior::smoker, 55);
    w[6] = 0; w[19] = 0; w[14] = w[15] = 0;
    w[10] = w[11] = w[12] = 0;
    w[26] = static_cast<int>(Behavior::dummy_craft);
    state_.weapons.buddies.fill(no_entity);
    for (int mount : {0,1,2,11,12,13,14,15,16,17,18,19,20}) {
        const int slot = state_.weapons.mounts[mount];
        if (slot == no_entity) { continue; }
        auto& object = entity(slot).words;
        object[26] = static_cast<int>(Behavior::debris_large);
        const auto vector = combat_jitter[state_.effects_phase];
        object[10] = vector[0] * 9;
        object[11] = std::abs(vector[1]) * 9;
        object[12] = vector[2] * 9;
        state_.effects_phase = (state_.effects_phase + 1) & 255;
        object[13] = combat_jitter[state_.effects_phase][0];
    }
    state_.finish = FinishKind::death;
    state_.finish_counter = 0;
}

void Combat::award(const EntityState& actor, int score) {
    const auto& owner = entity(entity(entity(actor.words[22]).words[23]).words[23]);
    if ((owner.words[1] & 0x20c) == 0x204) { state_.score += score; }
}

EntityState& Combat::add_object(EntityState& actor, int flags, int health, int model, Behavior behavior) {
    const int x = shift(actor.words[7], 8) * 256, z = shift(actor.words[9], 8) * 256;
    auto& result = spawn(actor, flags, health, model, behavior);
    const int next = result.words[0];
    result.words.fill(0);
    auto& w = result.words;
    w[0] = next; w[1] = flags; w[2] = health; w[5] = 10;
    w[7] = x; w[8] = height_(x, z) - word(resource_(10000 + model), 16) * 256; w[9] = z;
    w[16] = -1; w[19] = model; w[22] = result.slot; w[23] = result.slot;
    w[26] = static_cast<int>(behavior);
    return result;
}

void Combat::debris(EntityState& actor) {
    if (debris_models_.empty()) {
        const auto data = resource_(11004);
        std::size_t header = 0;
        while (header + 12 <= data.size() && word(data, header) != 0) {
            const int key = word(data, header + 8);
            const auto first = static_cast<std::size_t>(word(data, header + 4));
            auto position = first;
            do {
                debris_models_[key].push_back(word(data, position + 8));
                position = static_cast<std::size_t>(word(data, position + 4));
                if (debris_models_[key].size() > data.size() / 12) { throw std::runtime_error("Invalid debris cycle"); }
            } while (position != first);
            header = static_cast<std::size_t>(word(data, header));
        }
    }
    const int model = actor.words[19];
    const auto wreck = debris_models_.find(model);
    if (wreck != debris_models_.end()) {
        auto& cursor = debris_cursors_[model];
        auto& result = add_object(actor, 0x4041, 3000, wreck->second.at(cursor), Behavior::wreck);
        result.words[13] = actor.words[13];
        cursor = (cursor + 1) % wreck->second.size();
    }
    const int size = bound(model), speed = size < 1000 ? 3 : size < 5000 ? 5 : 8;
    state_.object_direction[0] = size;
    for (int group = 1; group <= 2; ++group) {
        const auto found = debris_models_.find(model + group * 65536);
        if (found == debris_models_.end()) { continue; }
        const int count = group == 1 ? static_cast<int>(found->second.size()) : speed;
        const int force = group == 1 ? speed : (speed + 1) / 2;
        for (int i = 0; i < count; ++i) {
            auto& result = spawn(actor, 0xc03b, 20, found->second[i % found->second.size()],
                group == 1 ? Behavior::debris_large : Behavior::debris_small);
            const auto vector = combat_jitter[state_.effects_phase];
            result.words[10] = vector[0] * force;
            result.words[11] = std::abs(vector[1]) * force;
            result.words[12] = vector[2] * force;
            state_.effects_phase = (state_.effects_phase + 1) & 255;
            result.words[13] = combat_jitter[state_.effects_phase][0];
        }
    }
}

void Combat::search(EntityState& actor, int interval, int radius, int mask, int bits) {
    auto& w = actor.words;
    if (w[22] != actor.slot) {
        interval *= 4;
    }
    if (w[6] <= interval) {
        return;
    }
    w[22] = actor.slot;
    w[6] = 0;
    std::vector<int> candidates;
    const auto append = [&](int head) {
        for (int slot = head; slot != no_entity; slot = entity(slot).words[0]) {
            if ((entity(slot).words[1] & mask) == bits) {
                candidates.push_back(slot);
            }
        }
    };
    const int x = shift(w[7], 17), z = shift(w[9], 17);
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            append(state_.grid[((z + dz) & 63) * 64 + ((x + dx) & 63)]);
        }
    }
    append(w[0]);
    int best = 400000;
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        const auto& target = entity(*it);
        int distance = 0;
        for (int i = 7; i <= 9; ++i) {
            const int delta = shift(target.words[i] - w[i], 10);
            distance += delta * delta;
        }
        if (distance < best) {
            best = distance;
            w[22] = target.words[23];
        }
    }
}

void Combat::aim(EntityState& actor) {
    auto& w = actor.words;
    const auto& target = entity(w[22]).words;
    state_.object_direction = {target[7] - w[7], target[9] - w[9]};
    int best = -999999999, selected = 0;
    for (int i = 0; i < static_cast<int>(aim_vectors.size()); ++i) {
        const auto direction = rotate(actor, aim_vectors[i]);
        std::int64_t product = 0;
        for (int axis = 0; axis < 3; ++axis) {
            product += static_cast<std::int64_t>(direction[axis]) * (target[7 + axis] - w[7 + axis]);
        }
        const auto score = static_cast<std::int32_t>(static_cast<std::uint32_t>(product));
        if (score > best) {
            best = score;
            selected = i;
        }
    }
    if (selected == 1) { w[13] -= 3; }
    if (selected == 2) { w[13] += 3; }
    if (selected == 3) { w[14] += 2; }
    if (selected == 4) { w[14] -= 2; }
    if (static_cast<Behavior>(w[26]) != Behavior::turret_top && selected == 0) {
        fire(actor);
    }
}

void Combat::align_attitude(EntityState& actor, const EntityState& reference) {
    auto& w = actor.words;
    const auto& base = reference.words;
    const int difference = w[13] - base[13], s = sine(difference), c = cosine(difference);
    const int pitch = base[14], roll = base[15];
    w[14] = shift(pitch * c - roll * s,8);
    w[15] = shift(roll * c + pitch * s,8);
}


void Combat::aircraft(EntityState& actor) {
    auto& w = actor.words;
    const bool pursuing = w[22] != actor.slot;
    if ((w[1] & 0x10) != 0 && pursuing && (entity(w[22]).words[1] & 4) == 0) { search(actor, 10); }
    if ((w[1] & 0x10) == 0) {
        search(actor, 70);
        if (w[22] != actor.slot) {
            w[6] = 0;
            w[1] |= 0x12;
        }
    } else if (pursuing && w[6] < 80) {
        w[13] += 2;
        w[11] += w[6] * 2;
        for (int i = 10; i <= 12; ++i) { w[i] = shift(w[i] * 14, 4); }
    } else {
        // The same helicopter body is used by the player and airborne enemies.
        int clearance = 80;
        if (w[22] != actor.slot) {
            const auto& target = entity(w[22]).words;
            const int dx = shift(w[7] - target[7], 12), dz = shift(w[9] - target[9], 12);
            const int distance = dx * dx + dz * dz;
            const bool airborne = (target[1] & 0x10) != 0;
            clearance = distance < 5000 ? shift(distance * (airborne ? 4 : 7), 8) + (airborne ? 70 : 10) : 150;
        }
        for (int i = 7; i <= 9; ++i) { w[i] -= w[i + 3]; }
        move_aircraft(actor, clearance, height_);
        --w[6];
        if (!pursuing) { search(actor, 20); }
        if (pursuing) {
            const auto& target = entity(w[22]).words;
            const int dx = shift(target[7], 8) - shift(w[7], 8), dz = shift(target[9], 8) - shift(w[9], 8);
            state_.object_direction = {dx, dz};
            int selected = 0, best = -999999999;
            for (int i = 0; i < 8; ++i) {
                const int score = sine(w[13] + i * 64) * dx + cosine(w[13] + i * 64) * dz;
                if (score > best) { best = score; selected = i; }
            }
            constexpr int yaw_steps[]{0,3,4,4,4,-4,-4,-3};
            constexpr int pitch_steps[]{-4,-3,0,3,4,3,0,-3};
            constexpr int bank_steps[]{0,5,6,5,0,-5,-6,-5};
            w[13] = (w[13] + yaw_steps[selected]) & 511;
            if (selected == 0) {
                int fine_best = -999999999, yaw = w[13];
                for (int offset : {-2,0,2}) {
                    const int score = sine(w[13] + offset) * dx + cosine(w[13] + offset) * dz;
                    if (score > fine_best) { fine_best = score; yaw = w[13] + offset; }
                }
                w[13] = yaw;
            }
            if ((pitch_steps[selected] < 0 && w[14] > -25) || (pitch_steps[selected] > 0 && w[14] < 25)) {
                w[14] += pitch_steps[selected];
            }
            if ((bank_steps[selected] < 0 && w[15] > -25) || (bank_steps[selected] > 0 && w[15] < 25)) {
                w[15] += bank_steps[selected];
            }
        }
    }
    if (w[2] < 1) {
        w[26] = static_cast<int>(Behavior::dead_aircraft);
        w[1] |= 0x4020;
        w[6] = 0;
        effect(actor, Behavior::smoker, 55);
        effect(actor, Behavior::aircraft_explosion, 13);
        award(actor, 210);
        ++state_.kills;
    }
}

void Combat::demo_helicopter(EntityState& actor) {
    auto& w = actor.words;
    if (w[16] == -1) { nearest_node(actor,NodeKind::air); }
    for (int axis = 7; axis <= 9; ++axis) { w[axis] -= w[axis + 3]; }
    move_aircraft(actor,80,height_);
    --w[6];
    const auto& node = nodes_.at(w[16]);
    const int x = node[4], z = node[5];
    const int dx = x - shift(w[7],8), dz = z - shift(w[9],8);
    constexpr int alignment_floor = -999999999;
    int selected = 0, best = alignment_floor;
    for (int direction = 0; direction < 8; ++direction) {
        const int score = sine(w[13] + direction * 64) * dx + cosine(w[13] + direction * 64) * dz;
        if (score > best) { best = score; selected = direction; }
    }
    w[13] &= 511;
    constexpr int yaw[]{0,3,4,4,4,-4,-4,-3};
    constexpr int pitch[]{-4,-3,0,3,4,3,0,-3};
    constexpr int bank[]{0,5,6,5,0,-5,-6,-5};
    constexpr int attitude_limit = 25;
    if ((pitch[selected] < 0 && w[14] > -attitude_limit) || (pitch[selected] > 0 && w[14] < attitude_limit)) {
        w[14] += pitch[selected];
    }
    if ((bank[selected] < 0 && w[15] > -attitude_limit) || (bank[selected] > 0 && w[15] < attitude_limit)) {
        w[15] += bank[selected];
    }
    if (selected == 0) {
        int heading = w[13];
        best = alignment_floor;
        for (int offset : {-2,0,2}) {
            const int score = sine(w[13] + offset) * dx + cosine(w[13] + offset) * dz;
            if (score > best) { best = score; heading = w[13] + offset; }
        }
        w[13] = heading;
    } else { w[13] += yaw[selected]; }
    state_.object_direction = {dx,dz};
    state_.node_range = dx * dx + dz * dz;
    if (state_.node_range >= 10000) { return; }
    const int current = w[16];
    int next = current;
    for (int i = 0; i < 4; ++i) {
        if (nodes_.at(current)[i] != -1) { next = nodes_.at(current)[i]; }
    }
    for (int i = 0; i < 4; ++i) {
        if (nodes_.at(next)[i] == current) { nodes_.at(next)[i] = -1; }
    }
    ++nodes_.at(current)[7];
    --nodes_.at(next)[7];
    w[16] = next;
}

int Combat::node_distance(int node, const EntityState& target, int previous, int depth, NodeKind kind) {
    const auto& n = nodes_.at(node);
    const int dx = n[4] - shift(target.words[7], 8), dz = n[5] - shift(target.words[9], 8);
    int best = dx * dx + dz * dz;
    if (depth < 5) {
        for (int i = 0; i < 4; ++i) {
            const int next = n[i];
            if (next != -1 && next != previous && nodes_.at(next)[static_cast<std::size_t>(kind)] > 0) {
                best = std::min(best, node_distance(next, target, node, depth + 1, kind));
            }
        }
    }
    return best + depth;
}

void Combat::change_node(EntityState& actor, NodeKind kind) {
    auto& w = actor.words;
    const int current = w[16];
    auto& node = nodes_.at(current);
    const auto capacity = static_cast<std::size_t>(kind);
    int selected = current;
    if (w[22] == actor.slot) {
        for (int i = 0; i < 3; ++i) {
            const int next = node[(w[6] + i) & 3];
            if (next != -1 && nodes_.at(next)[capacity] > 0) { selected = next; break; }
        }
    } else {
        ++node[capacity];
        const auto& target = entity(w[22]);
        const int dx = node[4] - shift(target.words[7], 8), dz = node[5] - shift(target.words[9], 8);
        int best = dx * dx + dz * dz;
        for (int i = 0; i < 4; ++i) {
            const int next = node[i];
            if (next == -1 || nodes_.at(next)[capacity] <= 0) { continue; }
            const int distance = node_distance(next, target, current, 1, kind);
            if (distance < best) { best = distance; selected = next; }
        }
        --node[capacity];
    }
    ++node[capacity];
    --nodes_.at(selected)[capacity];
    w[16] = selected;
}

void Combat::tank(EntityState& actor) {
    auto& w = actor.words;
    const bool boat = static_cast<Behavior>(w[26]) == Behavior::boat;
    const bool amphibious = static_cast<Behavior>(w[26]) == Behavior::amphibious_tank;
    const auto kind = boat ? NodeKind::water : NodeKind::land;
    const auto capacity = static_cast<std::size_t>(kind);
    if (nodes_.empty()) {
        const auto bytes = resource_(107);
        for (std::size_t offset = 0; offset + 36 <= bytes.size(); offset += 36) {
            std::array<int, 9> node{};
            for (int i = 0; i < 9; ++i) { node[i] = word(bytes, offset + i * 4); }
            if (node[6] == -1) { break; }
            nodes_.push_back(node);
        }
    }
    if (w[16] == -1) {
        int best = 999999999;
        for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) {
            const auto& node = nodes_[i];
            if (node[capacity] <= 0) { continue; }
            const int dx = node[4] - shift(w[7], 8), dz = node[5] - shift(w[9], 8);
            const int distance = dx * dx + dz * dz;
            if (distance < best) { best = distance; w[16] = i; }
        }
        if (w[16] == -1) { throw std::runtime_error("Tank has no available land node"); }
        --nodes_.at(w[16])[capacity];
    }
    if (w[1] == -1) { ++nodes_.at(w[16])[capacity]; return; }
    const int floor = word(resource_(10000 + w[19]), 16) * 256;
    const int sp = sine(-w[14]), cp = cosine(-w[14]), sr = sine(-w[15]), cr = cosine(-w[15]);
    const int sy = sine(-w[13]), cy = cosine(-w[13]);
    std::array<int, 4> wheels{};
    for (int i = 0; i < 4; ++i) {
        const int x = (i & 1) == 0 ? -5120 : 5120, z = i < 2 ? 7680 : -7680;
        const int pitched_y = shift(floor * cp - z * sp, 8), pitched_z = shift(floor * sp + z * cp, 8);
        const int rolled_x = shift(x * cr - pitched_y * sr, 8), y = shift(x * sr + pitched_y * cr, 8);
        const int world_x = shift(rolled_x * cy - pitched_z * sy, 8) + w[7];
        const int world_z = shift(rolled_x * sy + pitched_z * cy, 8) + w[9];
        wheels[i] = w[8] + y - surface_(world_x, world_z);
    }
    const auto momentum = rotate(actor, {{0,0,w[3] * 3}});
    if (!boat && !amphibious) { search(actor, 40); }
    const int minimum = *std::min_element(wheels.begin(), wheels.end());
    const int lowest = boat || amphibious ? minimum : std::min(1000, minimum);
    for (int i = 10; i <= 12; ++i) { w[i] = shift(w[i] * 14, 4); }
    if (lowest < 1000) {
        w[14] += shift(wheels[2] + wheels[3] - wheels[0] - wheels[1], 9);
        w[15] += shift(wheels[1] + wheels[3] - wheels[2] - wheels[0], 9);
    } else { w[14] -= 2; }
    if (lowest > 0) { w[11] -= 150; }
    if (lowest < 0) { w[11] = std::max(w[11], 0); w[8] -= lowest; }
    const auto& node = nodes_.at(w[16]);
    const int dx = node[4] - shift(w[7], 8), dz = node[5] - shift(w[9], 8);
    const int distance = dx * dx + dz * dz;
    state_.node_range = distance;
    state_.object_direction = {dx, dz};
    int yaw = w[13], best = -999999999;
    for (int offset : {-10,0,10}) {
        const int score = sine(w[13] + offset) * dx + cosine(w[13] + offset) * dz;
        if (score > best) { best = score; yaw = w[13] + offset; }
    }
    w[13] = yaw;
    if (boat || amphibious) { search(actor, 40, 2); }
    if (distance < 1000) { change_node(actor, kind); }
    if (lowest < 1000 && distance > 1000) {
        for (int i = 0; i < 3; ++i) { w[10+i] += momentum[i]; }
    }
    if (w[2] < 1) {
        ++nodes_.at(w[16])[capacity];
        award(actor, 59);
        if (boat || amphibious) { w[1] = 0; effect(actor, Behavior::explosion, 27); }
        else { destroy(actor); }
    }
}

void Combat::update(EntityState& actor) {
    auto& w = actor.words;
    const auto behavior = static_cast<Behavior>(w[26]);
    switch (behavior) {
    case Behavior::demo_helicopter: demo_helicopter(actor); break;
    case Behavior::hover:
    case Behavior::helicopter: {
        for (int i = 7; i <= 9; ++i) { w[i] -= w[i + 3]; }
        const auto impacts = state_.flight.ground_impacts;
        move_helicopter(actor, state_.flight, height_);
        if (state_.flight.ground_impacts != impacts) { effect(actor,Behavior::explosion,27); smoke(actor); }
        --w[6];
        break;
    }
    case Behavior::helicopter_change:
    case Behavior::buggy_change:
    case Behavior::ski_change:
    case Behavior::ground_buggy:
    case Behavior::ground_ski: vehicle_actor(actor); break;
    case Behavior::buggy_exit: vehicle_actor(actor); break;
    case Behavior::wheel: break;
    case Behavior::pose:
        if (w[6] == 0) { w[20] = w[2]; }
        if (w[2] > 0 && w[2] != w[20]) { w[20] = w[2]; effect(actor, Behavior::smoker, 55); }
        if (w[2] < 1) { destroy(actor); } else { w[1] |= frozen; }
        break;
    case Behavior::bullet:
    case Behavior::falling_bullet:
    case Behavior::enemy_bullet:
        if (behavior != Behavior::enemy_bullet) { --w[5]; }
        if (w[8] <= height_(w[7], w[9]) || w[2] < 1) {
            w[1] = 0;
            effect(actor, Behavior::bang, 39);
        }
        if (behavior != Behavior::enemy_bullet && w[5] == 0) { w[1] = 0; }
        if (behavior == Behavior::falling_bullet) { w[11] -= 210; }
        break;
    case Behavior::homing_bullet: {
        --w[5];
        if (w[8] <= height_(w[7], w[9]) || w[2] < 1) { w[1] = 0; effect(actor, Behavior::bang, 39); }
        if (w[5] == 0) { w[1] = 0; }
        const auto v = rotate(actor, {{0,0,4000}});
        face(actor, 2);
        std::copy(v.begin(), v.end(), w.begin() + 10);
        break;
    }
    case Behavior::bang:
    case Behavior::explosion:
    case Behavior::aircraft_explosion:
        if (w[6] > 1) { w[6] = 0; ++w[20]; }
        if (w[20] == (behavior == Behavior::bang ? 46 : behavior == Behavior::aircraft_explosion ? 27 : 39)) { w[1] = 0; }
        break;
    case Behavior::ground_explosion:
        if (w[6] == 0) { w[20] = 1; }
        if ((w[6] & 1) == 0) { ++w[20]; }
        if (w[20] == 13) { w[1] = 0; }
        break;
    case Behavior::smoker: {
        const auto& parent = entity(w[23]).words;
        std::copy(parent.begin() + 7, parent.begin() + 10, w.begin() + 7);
        if ((w[6] & 15) == 0) { smoke(actor); }
        if (w[6] > 50) { w[1] = 0; }
        break;
    }
    case Behavior::smoke:
        if (w[6] > 6) { w[6] = 0; ++w[20]; }
        if (w[20] == 56) { w[1] = 0; }
        w[10] += 5; w[11] -= 15;
        break;
    case Behavior::turret_top:
    case Behavior::tank_top:
        if (w[5] > 0) { --w[5]; }
        if (behavior == Behavior::tank_top) {
            const auto& parent = entity(w[23]).words;
            std::copy(parent.begin() + 7, parent.begin() + 10, w.begin() + 7);
            w[22] = parent[22];
        } else { search(actor, 40); }
        if (w[22] == actor.slot || w[22] == w[23]) {
            if (behavior == Behavior::turret_top && w[14] < 55) { ++w[14]; }
            const int yaw = entity(w[23]).words[13];
            if (yaw < w[13]) { --w[13]; }
            if (w[13] < yaw) { ++w[13]; }
        } else {
            aim(actor);
            if (behavior == Behavior::turret_top) {
                w[14] = std::clamp(w[14], -15, 55);
                fire(actor);
            }
        }
        if (w[2] < 1) {
            if (behavior == Behavior::turret_top) { destroy(actor); } else { w[1] = 0; }
        }
        if (w[23] == actor.slot) {
            w[11] -= 30; w[13] += 25;
            if (w[8] <= height_(w[7], w[9])) {
                w[1] = 0;
                if (behavior == Behavior::turret_top) { effect(actor, Behavior::explosion, 27); }
            }
        } else if (entity(w[23]).words[2] < 1) {
            w[11] = 1200; w[23] = actor.slot; w[1] |= 0x4012;
            if (behavior == Behavior::turret_top) { award(actor, 111); }
        }
        if (behavior == Behavior::tank_top) {
            const auto& parent = entity(w[23]).words;
            const int yaw = w[13] - parent[13];
            const int pitch = shift(parent[14] * cosine(yaw) - parent[15] * sine(yaw), 8);
            const int bank = shift(parent[14] * sine(yaw) + parent[15] * cosine(yaw), 8);
            w[14] = std::clamp(w[14], pitch - 10, pitch + 10);
            if (w[15] > bank) { w[15] = pitch; }
            if (w[15] < pitch) { w[15] = pitch; }
        }
        break;
    case Behavior::aircraft: aircraft(actor); break;
    case Behavior::dead_aircraft:
        w[13] += shift(w[6], 2); w[2] = 100; w[11] -= 50;
        w[10] = shift(w[10] * 31, 5); w[12] = shift(w[12] * 31, 5);
        if (w[8] <= height_(w[7], w[9])) { w[1] = 0; effect(actor, Behavior::explosion, 27); }
        break;
    case Behavior::debris_large:
    case Behavior::debris_small:
        w[11] -= 40;
        if (w[8] <= height_(w[7], w[9])) { w[1] = 0; effect(actor, Behavior::bang, 39); }
        w[2] = behavior == Behavior::debris_large ? 15 : 10;
        w[10] = shift(w[10] * 31, 5); w[12] = shift(w[12] * 31, 5);
        w[15] += behavior == Behavior::debris_large ? 3 : 6;
        w[13] += behavior == Behavior::debris_large ? 1 : 2;
        w[14] += behavior == Behavior::debris_large ? 7 : 11;
        break;
    case Behavior::tank:
    case Behavior::boat:
    case Behavior::amphibious_tank: tank(actor); break;
    case Behavior::boss_core:
    case Behavior::boss_base:
    case Behavior::boss_top:
    case Behavior::boss_air: boss(actor); break;
    case Behavior::boss2_core:
    case Behavior::boss2_top:
    case Behavior::pyramid:
    case Behavior::pyramid_gun:
    case Behavior::pyramid_eye:
    case Behavior::pyramid_shot:
    case Behavior::pyramid_dive:
    case Behavior::mars_face: later_boss(actor); break;
    case Behavior::drone:
    case Behavior::drone_gun:
    case Behavior::slug_drone:
    case Behavior::grav_tank:
    case Behavior::grav_top:
    case Behavior::carryall:
    case Behavior::hover_turret_base:
    case Behavior::hover_turret_top: lunar_actor(actor); break;
    case Behavior::stationary_tank:
    case Behavior::stationary_top:
    case Behavior::trailer:
    case Behavior::trailer_missile: mars_actor(actor); break;
    case Behavior::teleport: mars_actor(actor); break;
    case Behavior::pickup:
    case Behavior::secret_pickup: pickup(actor); break;
    case Behavior::mounted_weapon: {
        const auto& parent = entity(w[23]).words;
        std::copy(parent.begin() + 13, parent.begin() + 16, w.begin() + 13);
        w[10] = w[11] = w[12] = 0;
        break;
    }
    case Behavior::missile_single:
    case Behavior::missile_pair:
    case Behavior::enemy_missile:
    case Behavior::missile_coast: missile(actor); break;
    case Behavior::fire_trail:
        if (++w[20] == 27) { w[1] = 0; }
        break;
    case Behavior::shield: {
        auto& parent = entity(w[23]).words;
        if (w[20] > parent[2]) {
            w[2] -= w[20] - parent[2];
            parent[2] = w[20];
        }
        w[20] = parent[2];
        if (w[2] <= 0 || parent[2] == 0) { w[1] = 0; effect(actor, Behavior::explosion, 27); }
        w[13] += 3;
        w[14] += shift(w[2], 6);
        w[15] += shift(w[2], 5);
        break;
    }
    case Behavior::falling_casing:
    case Behavior::cruise_missile:
    case Behavior::scatter_missile:
    case Behavior::shard_bomb:
    case Behavior::pulse_ring:
    case Behavior::pulse_wall:
    case Behavior::cluster_missile:
    case Behavior::cluster_child:
    case Behavior::bomber_missile:
    case Behavior::carpet_missile:
    case Behavior::polaris_missile:
    case Behavior::rocket_buddy:
    case Behavior::tracking_buddy:
    case Behavior::bomber_buddy:
    case Behavior::buddy_bomb:
    case Behavior::bomblet: weapon_actor(actor); break;
    case Behavior::wreck: w[1] |= frozen; break;
    case Behavior::none: break;
    case Behavior::dummy_craft: w[10] = w[11] = w[12] = 0; break;
    case Behavior::apc_wheel:
    case Behavior::pillbox:
    case Behavior::tank_factory:
    case Behavior::plane_factory:
    case Behavior::radar:
    case Behavior::air_patrol:
    case Behavior::attack_plane:
    case Behavior::strafe_plane:
    case Behavior::strafe_factory:
    case Behavior::change_station:
    case Behavior::ski_station:
    case Behavior::ground_exit:
    case Behavior::submarine:
    case Behavior::parked_helicopter:
    case Behavior::burst_factory:
    case Behavior::intruder_factory:
    case Behavior::intruder:
    case Behavior::conditional_pickup:
    case Behavior::completion_pickup:
        mission_actor(actor);
        break;
    }
}

void Combat::collide(std::size_t index) {
    for (int a = state_.grid[index]; a != no_entity; a = entity(a).words[0]) {
        auto& aw = entity(a).words;
        if ((aw[1] & 0x40) != 0) { continue; }
        for (int b = aw[0]; b != no_entity; b = entity(b).words[0]) {
            auto& bw = entity(b).words;
            if (((aw[1] | bw[1]) & 2) == 0 || (bw[1] & 0x40) != 0 || a == bw[23] || b == aw[23] ||
                aw[23] == bw[23] || a == entity(bw[23]).words[23] || b == entity(aw[23]).words[23]) { continue; }
            std::array<int, 3> delta{};
            int distance = 0;
            for (int i = 0; i < 3; ++i) { delta[i] = shift(bw[7+i] - aw[7+i], 8); distance += delta[i] * delta[i]; }
            if (distance > bound(aw[19]) + bound(bw[19])) { continue; }
            const bool push_only = (aw[1] & 10) + (bw[1] & 10) == 2;
            if (!push_only) {
                aw[22] = bw[23]; bw[22] = aw[23];
                const int damage = std::min({aw[2], bw[2], 100});
                aw[2] -= damage; bw[2] -= damage;
                aw[1] = (aw[1] | 0x20000) & 0x7ffff;
                bw[1] = (bw[1] | 0x20000) & 0x7ffff;
            }
            // LSPC_normint uses the quantized square-root/reciprocal tables.
            for (int i = 0; i < 3; ++i) { delta[i] = shift(bw[7+i] - aw[7+i], 4); }
            delta = normalize(delta);
            const int force = push_only ? 4 : 8;
            for (int i = 0; i < 3; ++i) {
                const int normal = delta[i];
                if ((aw[1] & 2) != 0) { aw[10+i] -= normal * force; }
                if ((bw[1] & 2) != 0) { bw[10+i] += normal * force; }
            }
        }
    }
}

void Combat::advance(const KeyboardState& keys) {
    ++state_.game_counter;
    auto& player = entity(state_.player);
    const bool demo = player.words[26] == static_cast<int>(Behavior::demo_helicopter);
    std::vector<std::pair<std::size_t, int>> execution;
    const auto append_region = [&](const EntityState& center, int passes) {
        const int x = shift(center.words[7],17), z = shift(center.words[9],17);
        for (int dz = 0; dz < activity_side; ++dz) {
            for (int dx = 0; dx < activity_side; ++dx) {
                const auto index = static_cast<std::size_t>(((z + dz - activity_side / 2) & 63) * 64 + ((x + dx - activity_side / 2) & 63));
                const int original = activity[dz * activity_side + dx];
                const int mode = passes == 1 ? (original != 0) + 1 : original;
                if (mode == 2 || state_.grid[index] == no_entity) { continue; }
                const auto found = std::find_if(execution.begin(),execution.end(),[&](const auto& entry) { return entry.first == index; });
                if (found == execution.end()) { execution.emplace_back(index,mode); }
                else if (found->second != mode) { found->second = 0; }
            }
        }
    };
    append_region(player,2);
    auto regions = std::move(active_regions_);
    active_regions_.clear();
    for (auto region : regions) {
        append_region(entity(region.actor),region.passes);
        if (--region.passes != 0) { active_regions_.push_front(region); }
    }
    for (auto entry : execution) {
        auto index = entry.first;
        int slot = state_.grid[index];
        state_.grid[index] = no_entity;
        while (slot != no_entity) {
            auto& actor = entity(slot);
            auto& w = actor.words;
            const int next = w[0];
            if (entry.second == 0) {
                if ((w[1] & frozen) == 0) {
                    for (int i = 7; i <= 9; ++i) { w[i] += w[i + 3]; }
                    update(actor);
                    w[7] &= world_mask; w[9] &= world_mask;
                }
                ++w[6];
                if (w[1] == 0) { retire(actor); } else { index = cell(actor); link(actor); }
            } else {
                w[1] &= 0xdffff;
                if ((w[1] & 0x20) != 0) {
                    w[1] = -1;
                    update(actor);
                    if (w[1] == -1) { w[1] = 0; retire(actor); }
                } else { link(actor); }
            }
            slot = next;
        }
        if (entry.second == 0) { collide(index); }
    }
    constexpr int light_decay = 30;
    for (auto& light : state_.lighting) { light = std::max(0, light - light_decay); }
    if (state_.finish_counter >= 0) { ++state_.finish_counter; }
    if (!demo && !state_.extracting && state_.finish != FinishKind::death && state_.flight.control == PlayerControl::normal) {
        weapon_controls(keys);
        if (keys[state_.flight.bindings[6]]) { primary(player); }
        state_.weapons.primary_held = keys[state_.flight.bindings[6]] ? 1 : 0;
    }
    if (state_.extracting && (state_.flight.craft == CraftKind::buggy || state_.flight.craft == CraftKind::ski)) {
        finish_vehicle();
    } else if (state_.extracting) {
        auto& attitude = state_.flight.attitude;
        player.words[13] = shift(attitude[0], 8);
        player.words[14] = shift(attitude[1] - 1200, 8);
        player.words[15] = shift(attitude[2], 7);
        attitude[1] = shift((attitude[1] - 1200) * 14, 4);
        attitude[2] = shift(attitude[2] * 14, 4);
    } else if (!demo && state_.finish != FinishKind::death) {
        if (state_.flight.control == PlayerControl::normal &&
            (state_.flight.craft == CraftKind::helicopter || state_.flight.craft == CraftKind::hover)) {
            control_helicopter(player, state_.flight, keys, entity(player.words[22]));
        } else { vehicle_control(keys); }
    }
    if (!demo && state_.finish != FinishKind::death) { track_gun(player); special_update(); }
    if (state_.finish == FinishKind::success && state_.finish_counter == 25) {
        player.words[6] = 0; player.words[1] = 0x40;
        state_.extracting = true;
    }
    if ((state_.finish == FinishKind::success && state_.finish_counter == 55) ||
        (state_.finish == FinishKind::delayed_success && state_.finish_counter == 400)) {
        if (state_.finish == FinishKind::delayed_success) { cue_message(state_,20,0,0x1400,MessageMotion::held); }
        state_.phase = MissionPhase::completed;
    }
    if (state_.finish == FinishKind::death && state_.finish_counter == 35) { state_.phase = MissionPhase::failed; }
    previous_keys_ = keys;
    search_flight_target(state_);
    auto& shield = state_.weapons.shield;
    if (shield != no_entity) {
        auto& absorber = entity(shield).words;
        if (absorber[1] == 0) { shield = no_entity; }
        else { std::copy(player.words.begin() + 7, player.words.begin() + 10, absorber.begin() + 7); }
    }
    constexpr int buddy_offsets[]{-64,64,-128,128};
    for (std::size_t index = 0; index < state_.weapons.buddies.size(); ++index) {
        auto& slot = state_.weapons.buddies[index];
        if (slot == no_entity) { continue; }
        auto& buddy = entity(slot).words;
        if (buddy[1] == 0) { slot = no_entity; continue; }
        buddy[7] = player.words[7] + sine(player.words[13] + 128) * buddy_offsets[index];
        buddy[9] = player.words[9] + cosine(player.words[13] + 128) * buddy_offsets[index];
    }
    state_.display_score += std::min(state_.score - state_.display_score, 3);
    if (player.words[2] < 1 && state_.finish_counter == -1) { kill_player(player); }
    if ((state_.game_counter & 1) == 0) {
        int bar = std::clamp(shift(player.words[2], 5), 0, 31);
        if (bar > 15) { bar += 12; }
        if (state_.health_bar < bar) {
            if (++state_.health_bar == 16) { sound(player, 4001); }
        }
        if (state_.health_bar > bar) {
            if (--state_.health_bar == 26) { sound(player, 4001); }
        }
    }
    advance_ground_effects(state_.ground_effects);
    ++state_.tick;
    state_.landscape_clock.rotor += 10;
    ++state_.landscape_clock.water;
}

}
