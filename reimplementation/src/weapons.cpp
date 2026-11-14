#include "combat.hpp"
#include "combat_math.hpp"
#include "primary_patterns.hpp"
#include "game_text.hpp"

#include <stdexcept>

namespace swiv3d::detail {
namespace {

using namespace combat_math;

constexpr int pickup_base = 1000;
constexpr int repair_small = 100;
constexpr int repair_large = 101;
constexpr int repair_overcharge = 102;
constexpr int shield_pickup = 107;
constexpr int rocket_buddy_pickup = 105;
constexpr int rocket_buddy_model = 851;
constexpr int rocket_buddy_health = 250;
constexpr int tracking_buddy_pickup = 106;
constexpr int tracking_buddy_model = 852;
constexpr int bomber_buddy_pickup = 108;
constexpr int primary_upgrade = 110;
constexpr int objective_grant_base = 4000;
constexpr int objective_grant_count = 10;
constexpr int normal_health = 496;
constexpr int pickup_radius_squared = 10000;
constexpr int scan_radius_squared = 6000000;
constexpr int tether_airborne = 130;
constexpr int tether_ground = 70;
constexpr int collection_complete = 11;
constexpr int pickup_ground_margin = 60;
constexpr int pickup_turn = 5;
constexpr int projectile_flags = 0x3f;
constexpr int mounted_flags = 0x40;
constexpr int effect_flags = 0xe1;
constexpr int fire_trail_model = 11001;
constexpr int single_model = 800;
constexpr int pair_model = 801;
constexpr int shield_model = 853;
constexpr int shield_energy = 200;
constexpr int shield_refill = 300;
constexpr int missile_speed = 5000;
constexpr int single_deployment = 20;
constexpr int pair_deployment = 40;

enum WeaponPhase { deploy = 0, reload = 1, extending = 2, ready = 10, release = 11,
                   pair_delay = 12, pair_release = 13, holster = 20, retracting = 22, change = 30 };
enum class MountPose { moving, ready };
constexpr std::array<int, 20> ammo_grants{{6,3,2,3,2,2,2,1,2,1,2,2,2,3,2,1,2,1,2,80}};
constexpr std::array<int, 20> ammo_limits{{100,60,30,40,20,20,20,10,20,10,40,30,15,40,20,5,20,10,20,500}};
constexpr std::array<int, 20> craft_masks{{31,31,31,31,31,31,31,31,31,18,31,31,31,31,31,31,31,31,31,22}};
constexpr std::array<std::array<int, 3>, 5> tracking_vectors{{
    {{0,0,1000}}, {{-44,0,999}}, {{44,0,999}}, {{0,44,999}}, {{0,-44,999}}
}};

}

bool Combat::can_give(int grant) {
    const auto& player = entity(state_.player).words;
    if ((player[1] & mounted_flags) != 0) { return true; }
    if (grant >= 0 && grant < static_cast<int>(ammo_limits.size())) {
        return state_.weapons.ammunition[grant] < ammo_limits[grant];
    }
    if (grant == repair_small || grant == repair_large) { return player[2] < normal_health; }
    if (grant == repair_overcharge) { return player[2] < normal_health * 2; }
    if (grant == rocket_buddy_pickup || grant == tracking_buddy_pickup || grant == bomber_buddy_pickup) {
        return std::find(state_.weapons.buddies.begin(), state_.weapons.buddies.end(), no_entity) != state_.weapons.buddies.end();
    }
    if (grant >= primary_upgrade && grant <= primary_upgrade + 3) { return state_.weapons.primary_level < 19; }
    return true;
}

void Combat::give(int grant) {
    auto& weapon = state_.weapons;
    auto& player = entity(state_.player).words;
    if (grant >= 0 && grant < static_cast<int>(ammo_limits.size())) {
        weapon.ammunition[grant] = std::min(ammo_limits[grant], weapon.ammunition[grant] + ammo_grants[grant]);
        if (weapon.selected == no_entity && (craft_masks[grant] & (1 << static_cast<int>(state_.flight.craft))) != 0) {
            weapon.phase = deploy;
            weapon.requested = grant;
        }
        return;
    }
    if (grant == shield_pickup) {
        if (weapon.shield == no_entity) {
            auto& absorber = spawn(entity(state_.player), 0x14, shield_energy, shield_model, Behavior::shield);
            weapon.shield = absorber.slot;
            player[2] = std::max(player[2], shield_energy);
            absorber.words[20] = player[2];
        } else {
            entity(weapon.shield).words[2] = shield_refill;
            player[2] = std::min(normal_health * 2, player[2] + normal_health);
        }
        return;
    }
    if (grant == rocket_buddy_pickup || grant == tracking_buddy_pickup || grant == bomber_buddy_pickup) {
        const bool rocket = grant == rocket_buddy_pickup;
        const bool tracking = grant == tracking_buddy_pickup;
        const std::array<int,4> slot_order = rocket ? std::array<int,4>{0,1,2,3} : std::array<int,4>{1,0,3,2};
        state_.object_direction[0] = 0;
        for (int slot : slot_order) {
            if (weapon.buddies[slot] != no_entity) { ++state_.object_direction[0]; continue; }
            auto& buddy = spawn(entity(state_.player), 0x14, rocket ? rocket_buddy_health : 70,
                                rocket ? rocket_buddy_model : tracking ? tracking_buddy_model : 810,
                                rocket ? Behavior::rocket_buddy : tracking ? Behavior::tracking_buddy : Behavior::bomber_buddy);
            weapon.buddies[slot] = buddy.slot;
            buddy.words[8] += 80000;
            break;
        }
        return;
    }
    if (grant == repair_small || grant == repair_large || grant == repair_overcharge) {
        const int amount = grant == repair_small ? 49 : grant == repair_large ? 124 : normal_health;
        const int limit = grant == repair_small || grant == repair_large ? normal_health : normal_health * 2;
        player[2] = std::min(limit, player[2] + amount);
        return;
    }
    if (grant >= primary_upgrade && grant <= primary_upgrade + 3) {
        constexpr int limits[]{4,9,14,19};
        if (weapon.primary_level < limits[grant - primary_upgrade]) { ++weapon.primary_level; }
        return;
    }
    if (grant >= objective_grant_base && grant < objective_grant_base + objective_grant_count) { return; }
    throw std::runtime_error("Untranslated pickup grant " + std::to_string(grant));
}

int Combat::face(EntityState& actor, int turn) {
    auto& w = actor.words;
    const auto& target = entity(w[22]).words;
    state_.object_direction = {target[7] - w[7], target[9] - w[9]};
    int best = -999999999, selected = 0;
    for (int i = 0; i < static_cast<int>(tracking_vectors.size()); ++i) {
        const auto direction = rotate(actor, tracking_vectors[i]);
        std::int64_t product = 0;
        for (int axis = 0; axis < 3; ++axis) {
            product += static_cast<std::int64_t>(direction[axis]) * (target[7+axis] - w[7+axis]);
        }
        const auto score = static_cast<std::int32_t>(static_cast<std::uint32_t>(product));
        if (score > best) { best = score; selected = i; }
    }
    if (selected == 1) { w[13] -= turn; }
    if (selected == 2) { w[13] += turn; }
    if (selected == 3) { w[14] += turn; }
    if (selected == 4) { w[14] -= turn; }
    return selected;
}

void Combat::pickup(EntityState& actor) {
    auto& w = actor.words;
    const int grant = w[21] - pickup_base;
    if (w[20] == 0) {
        ++w[5];
        w[8] = height_(w[7], w[9]) + 6000 + sine(w[5] * 4) * 8;
        w[13] += shift(sine(w[5] * 4), 4);
        if ((w[6] & 31) == 0) {
            ++state_.game_counter;
            const auto& player = entity(state_.player).words;
            const int dx = shift(w[7] - player[7], 8), dz = shift(w[9] - player[9], 8);
            state_.object_direction = {dx, dz};
            w[22] = (player[1] & mounted_flags) == 0 && dx * dx + dz * dz < scan_radius_squared ? state_.player : actor.slot;
        }
        if (w[22] == actor.slot) { return; }
        const auto& target = entity(w[22]).words;
        const int dx = shift(w[7] - target[7], 8), dz = shift(w[9] - target[9], 8);
        state_.object_direction = {dx, dz};
        if (dx * dx + dz * dz >= pickup_radius_squared || !can_give(grant)) { return; }
        if ((target[1] & mounted_flags) == 0 && ((grant >= 0 && grant < 20) || (grant >= 100 && grant <= 108) ||
                                               (grant >= primary_upgrade && grant <= primary_upgrade + 3))) {
            const int colour = grant < 20 ? 0x1000 : grant >= primary_upgrade ? 0x100 : 0x400;
            cue_message(state_,100,grant,colour);
        }
        w[23] = w[22];
        w[13] &= 511;
        const bool airborne = (target[1] & 0x10) != 0;
        w[20] = airborne ? 1 : 2;
        w[5] = airborne ? tether_airborne : tether_ground;
        if (airborne) { w[1] |= 0x400; }
        sound(actor, airborne ? 4000 : 4002);
        return;
    }
    if (w[20] != 1 && w[20] != 2) { return; }
    if (w[5] > 0) { --w[5]; }
    auto& player = entity(w[23]).words;
    std::array<int, 3> normal{};
    for (int i = 0; i < 3; ++i) { normal[i] = shift(player[7+i] - w[7+i], 6); }
    normal = normalize(normal);
    std::array<int, 3> position{};
    for (int i = 0; i < 3; ++i) { position[i] = player[7+i] - w[5] * normal[i]; }
    if (w[20] == 1) {
        if ((player[1] & mounted_flags) == 0) {
            for (int i = 0; i < 3; ++i) { player[10+i] -= shift(normal[i], 2); }
            w[11] -= 100;
        } else { w[5] = 0; }
        std::copy(position.begin(), position.end(), w.begin() + 7);
        if (w[8] - pickup_ground_margin < height_(w[7], w[9])) { w[5] -= 3; }
        face(actor, pickup_turn);
    } else {
        for (int i = 0; i < 3; ++i) { player[10+i] -= shift(normal[i], 2); }
        w[11] += 170;
        for (int i = 0; i < 3; ++i) { w[10+i] += shift(position[i] - w[7+i], 3); }
        std::copy(position.begin(), position.end(), w.begin() + 7);
    }
    if (w[5] >= collection_complete) { return; }
    w[1] = 0;
    give(grant);
    if (static_cast<Behavior>(w[26]) == Behavior::secret_pickup) { ++state_.weapons.secrets; }
    else { ++state_.weapons.collected; }
    check_objective(actor);
}

void Combat::reveal_pickup(EntityState& actor) {
    if (actor.words[21] < pickup_base || actor.words[21] >= 2000) { return; }
    const auto commands = resource_(105);
    const auto offset = static_cast<std::size_t>(actor.words[25]);
    const int model = word(commands, offset + 20);
    auto& result = add_object(actor, 0x41, 1000, model, Behavior::secret_pickup);
    result.words[13] = word(commands, offset + 12);
    result.words[21] = actor.words[21];
}

void Combat::primary(EntityState& player) {
    if (state_.weapons.primary_level < 0 || state_.weapons.primary_level > 19) {
        throw std::runtime_error("Invalid primary gun level");
    }
    const auto phase = (state_.flight.control_ticks + 1) % primary_periods[state_.weapons.primary_level];
    for (const auto& shot : primary_shots) {
        if (shot.level != state_.weapons.primary_level || shot.phase != static_cast<int>(phase)) { continue; }
        constexpr int models[]{10002,10000,10001,10003,800};
        constexpr int lifetimes[]{28,30,40,60,60};
        const auto behavior = shot.kind == 2 ? Behavior::falling_bullet : shot.kind > 2 ? Behavior::homing_bullet : Behavior::bullet;
        auto& bullet = spawn(player, projectile_flags, shot.damage, models[shot.kind], behavior);
        bullet.words[5] = lifetimes[shot.kind];
        bullet.words[13] = player.words[13] + shot.yaw;
        bullet.words[14] = state_.gun_pitch + shot.pitch;
        if (shot.kind < 3) {
            const auto v = rotate(bullet, {{shot.sideways,shot.vertical,shot.speed}});
            std::copy(v.begin(), v.end(), bullet.words.begin() + 10);
        }
        ++state_.shots;
    }
}

void Combat::select_weapon(int selection) {
    auto& weapon = state_.weapons;
    const auto available = [&](int slot) { return weapon.ammunition[slot] != 0 && (craft_masks[slot] & (1 << static_cast<int>(state_.flight.craft))) != 0; };
    if (selection < 0) {
        if (weapon.requested == no_entity) { return; }
        int slot = weapon.requested;
        for (int count = 0; count < 21; ++count) {
            slot = (slot + (selection == -1 ? 1 : 19)) % 20;
            if (available(slot)) { weapon.requested = slot; return; }
        }
        weapon.requested = no_entity;
        return;
    }
    const int digit = weapon.requested % 10;
    const int bank = digit == selection ? ((weapon.requested / 10 + 1) & 1) : 0;
    const int slot = selection + bank * 10;
    if (available(slot)) { weapon.requested = slot; return; }
    if (available(selection + 10)) { weapon.requested = selection + 10; }
    if (available(selection)) { weapon.requested = selection; }
}

void Combat::weapon_controls(const KeyboardState& keys) {
    const auto pressed = [&](std::size_t scan) { return keys[scan] && !previous_keys_[scan]; };
    for (int digit = 0; digit < 10; ++digit) {
        if (pressed(static_cast<std::size_t>(digit + 2))) { select_weapon(digit); }
    }
    if (pressed(state_.flight.bindings[8])) { select_weapon(-1); }
    if (pressed(state_.flight.bindings[9])) { select_weapon(-2); }
    if (keys[state_.flight.bindings[7]] && state_.weapons.phase == ready) { state_.weapons.phase = release; }
}

void Combat::place_weapon(int mount, int x, int y, int z) {
    place(entity(state_.player), entity(state_.weapons.mounts.at(mount)), x, y, z);
}

void Combat::place(EntityState& parent, EntityState& object, int x, int y, int z) {
    const auto& player = parent.words;
    auto& actor = object.words;
    const int sp = sine(-player[14]), cp = cosine(-player[14]);
    const int sr = sine(-player[15]), cr = cosine(-player[15]);
    const int sy = sine(-player[13]), cy = cosine(-player[13]);
    const int py = shift(y * cp - z * sp, 8), pz = shift(y * sp + z * cp, 8);
    const int rx = shift(x * cr - py * sr, 8), ry = shift(x * sr + py * cr, 8);
    actor[7] = player[7] + shift(rx * cy - pz * sy, 8);
    actor[8] = player[8] + ry;
    actor[9] = player[9] + shift(rx * sy + pz * cy, 8);
}

void Combat::special_update() {
    auto& weapon = state_.weapons;
    const auto has_ammo = [&](int slot) { return slot >= 0 && slot < 20 && weapon.ammunition[slot] != 0; };
    if (weapon.phase == release && weapon.selected != no_entity) { --weapon.ammunition[weapon.selected]; }
    if ((weapon.phase == deploy || weapon.phase == reload) && !has_ammo(weapon.selected)) { weapon.phase = change; }
    if (weapon.selected != weapon.requested && weapon.phase == ready) { weapon.phase = holster; }
    if (state_.flight.weapon_hold && weapon.phase == ready) { weapon.phase = holster; }
    if (weapon.selected != no_entity) {
        if (weapon.selected > 1) { special_family(); } else {
            const bool pair = weapon.selected == 1;
            const bool ground = state_.flight.craft == CraftKind::buggy || state_.flight.craft == CraftKind::ski;
            const bool hover = state_.flight.craft == CraftKind::hover;
            const int deployment = pair ? pair_deployment : single_deployment;
            const int side = ground ? 125 : hover ? 200 : 100;
            const auto place = [&](int progress, MountPose pose) {
                if (pair) {
                    const int y = ground ? 50 * progress : pose == MountPose::ready ? -700 : -17 * progress;
                    place_weapon(0,progress * side,y,0); place_weapon(1,-progress * side,y,0);
                } else { place_weapon(0,0,progress * (ground ? 200 : -150),0); }
            };
            const auto launch = [&](int mount) {
                auto& actor = entity(weapon.mounts[mount]);
                actor.words[1] = projectile_flags;
                actor.words[22] = entity(state_.player).words[22];
                actor.words[26] = static_cast<int>(pair ? Behavior::missile_pair : Behavior::missile_single);
                if (!pair && !ground) { actor.words[6] = 0; }
                sound(actor, 2000);
                ++state_.shots;
            };
            switch (weapon.phase) {
            case deploy:
            case reload: {
                const bool deploy_sound = weapon.phase == deploy || (!ground && !hover);
                auto& player = entity(state_.player);
                weapon.mounts[0] = spawn(player, mounted_flags, pair ? 100 : 80, pair ? pair_model : single_model, Behavior::mounted_weapon).slot;
                if (pair) { weapon.mounts[1] = spawn(player, mounted_flags, 100, pair_model, Behavior::mounted_weapon).slot; }
                weapon.progress = 0; weapon.phase = extending;
                if (deploy_sound) { sound(player, 3000); }
                break;
            }
            case extending:
                ++weapon.progress;
                place(weapon.progress,MountPose::moving);
                if (weapon.progress == deployment) { weapon.phase = ready; }
                break;
            case ready:
                place(deployment,MountPose::ready);
                break;
            case release:
                launch(0);
                weapon.phase = pair ? pair_delay : reload;
                if (pair) { weapon.progress = 7; }
                break;
            case pair_delay:
                if (--weapon.progress == 0) { weapon.phase = pair_release; }
                place_weapon(1,-pair_deployment * side,ground ? 2000 : -700,0);
                break;
            case pair_release:
                launch(1);
                weapon.phase = reload; weapon.progress = pair_deployment;
                break;
            case holster:
                if (ground || hover) { sound(entity(state_.player),3001); }
                weapon.phase = retracting; break;
            case retracting:
                --weapon.progress;
                place(weapon.progress,MountPose::moving);
                if (weapon.progress == 0) {
                    entity(weapon.mounts[0]).words[1] = 0; weapon.mounts[0] = no_entity;
                    if (pair) { entity(weapon.mounts[1]).words[1] = 0; weapon.mounts[1] = no_entity; }
                    weapon.phase = change;
                }
                break;
            case change: break;
            default: throw std::runtime_error("Untranslated special weapon transition");
            }
        }
    }
    if (weapon.phase != change || state_.flight.weapon_hold) { return; }
    int next = weapon.requested;
    if (!has_ammo(next)) {
        next = no_entity;
        for (int slot = 0; slot < 20; ++slot) {
            if (has_ammo(slot) && (craft_masks[slot] & (1 << static_cast<int>(state_.flight.craft))) != 0) { next = slot; break; }
        }
    }
    weapon.selected = weapon.requested = next;
    weapon.phase = deploy;
}

void Combat::missile(EntityState& actor) {
    auto& w = actor.words;
    const auto behavior = static_cast<Behavior>(w[26]);
    if (w[8] <= height_(w[7], w[9]) || w[2] < 1) { w[1] = 0; effect(actor, Behavior::bang, 39); }
    if (behavior != Behavior::missile_coast) {
        velocity(actor, missile_speed);
        if (entity(w[22]).words[1] == 0) { w[22] = w[23]; }
        if (w[22] != w[23] && w[22] != actor.slot && w[6] < 6) {
            face(actor, behavior == Behavior::missile_single ? 2 : behavior == Behavior::enemy_missile ? 1 : 4);
            w[15] = 0;
        }
    }
    const int trail_interval = behavior == Behavior::missile_single ? 4 : behavior == Behavior::missile_pair ? 3 : behavior == Behavior::enemy_missile ? 6 : 7;
    if (w[6] > trail_interval) {
        auto& trail = spawn(actor, effect_flags, 100, fire_trail_model, Behavior::fire_trail);
        trail.words[10] = trail.words[11] = trail.words[12] = 0;
        trail.words[20] = 13;
        w[6] = 0;
    }
    if (behavior != Behavior::missile_coast && w[5]++ > 100) { w[26] = static_cast<int>(Behavior::missile_coast); }
}

}
