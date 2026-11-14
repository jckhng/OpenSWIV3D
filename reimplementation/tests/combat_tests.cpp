#include "../src/combat.hpp"
#include "../src/game_text.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

struct World {
    swiv3d::MissionState state;
    World() {
        state.player = 3000;
        state.free_slot = 2999;
        state.grid.fill(swiv3d::no_entity);
        state.flight.bindings[6] = 0x39;
        add(3000, swiv3d::Behavior::helicopter, 0x217, 496);
    }
    swiv3d::EntityState& add(int slot, swiv3d::Behavior behavior, int flags, int health) {
        state.entities.push_back({slot, {}});
        auto& actor = state.entities.back();
        auto& w = actor.words;
        w[1] = flags; w[2] = health; w[7] = 16384; w[8] = 20480; w[9] = 16384;
        w[16] = -1; w[19] = 990; w[22] = slot; w[23] = slot; w[26] = static_cast<int>(behavior);
        w[0] = state.grid[0]; state.grid[0] = slot;
        if (slot != 3000) { state.free_slot = std::min(state.free_slot, slot - 1); }
        return actor;
    }
    swiv3d::detail::Combat simulation() {
        return swiv3d::detail::Combat(state, [](int, int) { return 0; }, [](std::uint32_t) {
            std::vector<std::uint8_t> model(32);
            model[12] = 100;
            return model;
        });
    }
};

void test_direction_scratch_alias() {
    World world;
    world.state.object_direction = {65536, 3};
    auto& missile = world.add(2999, swiv3d::Behavior::polaris_missile, 0x40, 100);
    missile.words[5] = 30;
    missile.words[22] = 3000;
    auto simulation = world.simulation();
    simulation.advance({});
    require(world.state.node_range == 9, "Direction test lost the prior shared X/Z values or 32-bit square wrap");
}

void test_ski_water_contacts() {
    for (const auto wet : {false,true}) {
        World world;
        auto& player = world.state.entities.front().words;
        player[1] = 0x207;
        player[8] = 0;
        player[19] = 991;
        player[26] = static_cast<int>(swiv3d::Behavior::ground_ski);
        world.state.flight.craft = swiv3d::CraftKind::ski;
        world.state.flight.control = swiv3d::PlayerControl::disabled;
        world.state.water_flags = wet ? 2 : 0;
        swiv3d::detail::Combat simulation(world.state,[](int,int) { return 0; },[](std::uint32_t id) {
            return std::vector<std::uint8_t>(id == 102 ? 512 * 512 : 32);
        });
        simulation.advance({});
        require(world.state.entities.front().words[14] == (wet ? 7 : 0),
                "Ski water contact lost the rear-wheel offset");
    }
}

void test_changeover_scan_counter() {
    for (const auto behavior : {swiv3d::Behavior::change_station, swiv3d::Behavior::ski_station}) {
        for (const int phase : {0, 5}) {
            World world;
            constexpr unsigned carried_counter = 60;
            world.state.game_counter = carried_counter;
            auto& station = world.add(2999, behavior, 0x40, 1000);
            station.words[7] += 1000 * 256;
            station.words[20] = phase;
            auto simulation = world.simulation();
            simulation.advance({});
            require(world.state.game_counter == carried_counter + 2 && world.state.health_bar == 1,
                    "Changeover scan lost its counter increment or health-bar step");
            simulation.advance({});
            require(world.state.game_counter == carried_counter + 3 && world.state.health_bar == 1,
                    "Changeover incremented the counter between proximity scans");
        }
    }
}

void test_ground_engine_scratch_alias() {
    World world;
    auto& player = world.state.entities.front().words;
    player[1] = 0x207;
    player[8] = 0;
    player[19] = 991;
    player[26] = static_cast<int>(swiv3d::Behavior::ground_ski);
    world.state.flight.craft = swiv3d::CraftKind::ski;
    world.state.flight.control = swiv3d::PlayerControl::disabled;
    world.state.flight.engine_pitch = 11000;
    world.state.object_direction = {123,3};
    auto& missile = world.add(2999,swiv3d::Behavior::polaris_missile,0x40,100);
    missile.words[5] = 30;
    missile.words[22] = 3000;
    missile.words[0] = swiv3d::no_entity;
    world.state.entities.front().words[0] = missile.slot;
    world.state.grid[0] = 3000;
    auto simulation = world.simulation();
    simulation.advance({});
    require(world.state.node_range == 16000009,
            "Ground engine update lost its shared X-scratch write");
}

void test_message_lifetime() {
    swiv3d::MissionState state;
    swiv3d::detail::cue_message(state,20,9,0x1400);
    require(state.messages.front().position == std::array<int,3>{-384000,140000,-420000},"Flying message lost its pre-roll");
    for (int tick = 0; tick < 70; ++tick) { swiv3d::detail::advance_messages(state); }
    require(state.messages.size() == 1 && state.messages.front().lifetime == 0,"Flying message retired early");
    swiv3d::detail::advance_messages(state);
    require(state.messages.empty(),"Expired flying message retained its pool slot");
    swiv3d::detail::cue_message(state,20,0,0x1400,swiv3d::detail::MessageMotion::held);
    for (int tick = 0; tick < 80; ++tick) { swiv3d::detail::advance_messages(state); }
    const auto position = state.messages.front().position;
    for (int tick = 0; tick < 10; ++tick) { swiv3d::detail::advance_messages(state); }
    require(state.messages.front().lifetime == 1000 && state.messages.front().position == position,"Held message moved past its hold boundary");

}

void test_primary_bursts_and_expiration() {
    World world;
    auto simulation = world.simulation();
    swiv3d::KeyboardState keys{};
    keys[0x39] = true;
    simulation.advance(keys);
    simulation.advance(keys);
    require(world.state.shots == 0, "Primary gun fired before its burst boundary");
    simulation.advance(keys);
    require(world.state.shots == 2 && world.state.free_slot == 2997, "Primary burst did not allocate two bullets");
    for (int slot : {2999,2998}) {
        const auto& w = std::find_if(world.state.entities.begin(), world.state.entities.end(),
            [&](const auto& actor) { return actor.slot == slot; })->words;
        require(w[5] == 28 && w[6] == 0 && w[12] == 8000 && w[23] == 3000,
                "New bullet moved early, inherited velocity or lost its owner");
    }
    keys.fill(false);
    for (int i = 0; i < 28; ++i) { simulation.advance(keys); }
    require(world.state.entities[1].words[1] == 0 && world.state.entities[2].words[1] == 0,
            "Primary bullets outlived their 28 body updates");
}

void test_damage_and_owner_exclusion() {
    World world;
    world.add(2999, swiv3d::Behavior::pose, 1, 100);
    auto& bullet = world.add(2998, swiv3d::Behavior::bullet, 0x3f, 10);
    bullet.words[23] = 3000;
    bullet.words[5] = 28;
    bullet.words[19] = 10002;
    auto simulation = world.simulation();
    simulation.advance({});
    require(world.state.entities[0].words[2] == 496, "Gun damaged its owner");
    require(world.state.entities[1].words[2] == 90 && world.state.entities[2].words[2] == 0,
            "Projectile damage did not use the smaller health payload");
    simulation.advance({});
    const auto& hit = world.state.entities[1].words;
    require(hit[20] == 90 && (hit[1] & 0x80000) != 0, "Hit did not wake the frozen scenery callback");
    require(std::any_of(world.state.entities.begin(), world.state.entities.end(), [](const auto& actor) {
        return static_cast<swiv3d::Behavior>(actor.words[26]) == swiv3d::Behavior::bang && actor.words[19] == 11000;
    }), "Spent projectile did not create its original impact effect");
}

void test_turret_acquisition_and_return_fire() {
    World world;
    auto& base = world.add(2999, swiv3d::Behavior::pose, 1, 100);
    base.words[7] += 32768;
    auto& turret = world.add(2998, swiv3d::Behavior::turret_top, 0x1001, 100);
    turret.words[7] += 32768;
    turret.words[13] = 384;
    turret.words[6] = 41;
    turret.words[21] = 100;
    turret.words[23] = 2999;
    auto simulation = world.simulation();
    simulation.advance({});
    require(world.state.entities[2].words[22] == 3000, "Enemy failed to acquire the player");
    require(world.state.shots == 1 && world.state.entities[2].words[5] == 50, "Enemy gun cadence changed");
    const auto& shot = world.state.entities.back().words;
    require(shot[10] == -9000 && shot[2] == 35 && shot[19] == 10005 && shot[23] == 2998,
            "Enemy gun speed, damage, model or owner changed");
}

void test_aim_clamp_and_idle_tank_top() {
    World world;
    world.state.entities[0].words[8] = 400000;
    auto& base = world.add(2999, swiv3d::Behavior::none, 1, 100);
    base.words[7] += 32768;
    auto& turret = world.add(2998, swiv3d::Behavior::turret_top, 0x1001, 100);
    turret.words[7] += 32768;
    turret.words[13] = 384;
    turret.words[14] = 55;
    turret.words[21] = 100;
    turret.words[22] = 3000;
    turret.words[23] = 2999;
    auto simulation = world.simulation();
    simulation.advance({});
    require(world.state.entities.back().words[14] == 55, "Turret fired before clamping its aim pitch");

    World tank_world;
    auto& hull = tank_world.add(2999, swiv3d::Behavior::none, 0x1003, 100);
    hull.words[7] += 32768;
    hull.words[13] = -290;
    auto& top = tank_world.add(2998, swiv3d::Behavior::tank_top, 0x1003, 100);
    top.words[7] += 32768;
    top.words[23] = 2999;
    auto tank_simulation = tank_world.simulation();
    tank_simulation.advance({});
    require(tank_world.state.entities[2].words[13] == -1 && tank_world.state.entities[2].words[14] == 0,
            "Idle tank top failed to follow its hull heading");
}

void test_pool_reuses_canonical_slots() {
    World world;
    for (int slot : {1,0}) {
        auto& bullet = world.add(slot, swiv3d::Behavior::bullet, 0x3f, 10);
        bullet.words[5] = 1;
        bullet.words[19] = 10002;
        bullet.words[23] = 3000;
    }
    auto simulation = world.simulation();
    simulation.advance({});
    require(world.state.free_slot == 0, "Retired objects did not join the free queue in update order");
    swiv3d::KeyboardState keys{};
    keys[0x39] = true;
    simulation.advance(keys);
    simulation.advance(keys);
    require(world.state.entities.size() == 3 && world.state.free_slot == swiv3d::no_entity,
            "Reusing slots duplicated entity storage or lost the free queue");
    require(world.state.entities[1].words[6] == 0 && world.state.entities[2].words[6] == 0,
            "Reused bullets retained their prior lifetime");
}

void test_pickup_collection_and_special() {
    World world;
    world.state.flight.bindings[7] = 0x32;
    auto& pickup = world.add(2999, swiv3d::Behavior::pickup, 0x41, 10);
    pickup.words[21] = 1000;
    auto simulation = world.simulation();
    simulation.advance({});
    require(world.state.entities[1].words[20] == 1 && world.state.entities[1].words[23] == 3000,
            "Nearby missile pickup did not start its collection tether");
    for (int i = 0; i < 150; ++i) { simulation.advance({}); }
    require(world.state.weapons.ammunition[0] == 6 && world.state.weapons.collected == 1,
            "Collected pickup did not grant its six missiles exactly once");
    require(world.state.weapons.selected == 0 && world.state.weapons.phase == 10,
            "Granted missile was not equipped through its deployment transition");
    swiv3d::KeyboardState keys{};
    keys[0x32] = true;
    simulation.advance(keys);
    require(world.state.weapons.ammunition[0] == 5 && world.state.weapons.phase == 1,
            "Special firing did not consume one missile at release");
}

void test_gun_upgrade_and_full_health_pickup() {
    World world;
    auto& pickup = world.add(2999, swiv3d::Behavior::pickup, 0x41, 10);
    pickup.words[21] = 1110;
    auto simulation = world.simulation();
    for (int i = 0; i < 160; ++i) { simulation.advance({}); }
    require(world.state.weapons.primary_level == 1, "Gun upgrade did not advance the primary level");

    World healthy;
    healthy.add(2999, swiv3d::Behavior::pickup, 0x41, 10).words[21] = 1101;
    auto health_simulation = healthy.simulation();
    health_simulation.advance({});
    require(healthy.state.entities[1].words[20] == 0, "Full-health player consumed a repair pickup");
}

void test_upgraded_burst_and_paired_release() {
    World world;
    world.state.weapons.primary_level = 3;
    auto simulation = world.simulation();
    swiv3d::KeyboardState keys{};
    keys[0x39] = true;
    for (int i = 0; i < 8; ++i) { simulation.advance(keys); }
    require(world.state.shots == 2 && world.state.entities[1].words[10] == 200 && world.state.entities[2].words[10] == -200,
            "Upgraded gun's secondary burst spread changed");

    World paired;
    paired.state.weapons.ammunition[1] = 3;
    paired.state.weapons.requested = 1;
    paired.state.flight.bindings[7] = 0x32;
    auto pair_simulation = paired.simulation();
    for (int i = 0; i < 45; ++i) { pair_simulation.advance({}); }
    require(paired.state.weapons.phase == 10, "Paired missiles did not finish deployment");
    keys.fill(false); keys[0x32] = true;
    pair_simulation.advance(keys);
    require(paired.state.weapons.ammunition[1] == 2 && paired.state.weapons.phase == 12,
            "Paired release did not consume one pair at the first missile");
    for (int i = 0; i < 7; ++i) { pair_simulation.advance({}); }
    require(paired.state.weapons.phase == 13 && paired.state.shots == 1, "Second missile released before its delay completed");
    pair_simulation.advance({});
    require(paired.state.weapons.phase == 1 && paired.state.shots == 2 && paired.state.weapons.ammunition[1] == 2,
            "Second missile consumed another ammunition unit or failed to release");
}

void test_selection_direction_and_shield() {
    World world;
    world.state.flight.bindings[8] = 0xc9;
    world.state.flight.bindings[9] = 0xd1;
    world.state.weapons.ammunition[0] = 6;
    world.state.weapons.ammunition[1] = 3;
    world.state.weapons.ammunition[2] = 2;
    auto simulation = world.simulation();
    for (int i = 0; i < 25; ++i) { simulation.advance({}); }
    swiv3d::KeyboardState keys{};
    keys[0xc9] = true;
    simulation.advance(keys);
    require(world.state.weapons.requested == 1, "Page Up did not increment the original weapon slot order");
    simulation.advance({});
    keys.fill(false); keys[0xd1] = true;
    simulation.advance(keys);
    require(world.state.weapons.requested == 0, "Page Down did not decrement the original weapon slot order");

    World shielded;
    shielded.state.entities[0].words[2] = 100;
    auto& pickup = shielded.add(2999, swiv3d::Behavior::pickup, 0x441, 10);
    pickup.words[21] = 1107; pickup.words[20] = 1; pickup.words[5] = 11;
    pickup.words[22] = pickup.words[23] = 3000;
    auto shield_simulation = shielded.simulation();
    shield_simulation.advance({});
    require(shielded.state.weapons.shield != swiv3d::no_entity && shielded.state.entities[0].words[2] == 200,
            "Shield pickup did not create its absorber or apply its minimum health");
    shielded.state.entities[0].words[2] = 150;
    shield_simulation.advance({});
    require(shielded.state.entities[0].words[2] == 200 && shielded.state.entities.back().words[2] == 150,
            "Shield failed to absorb the player's lost health on its callback");
}

void test_grant_limits_and_shield_refill() {
    const auto tether = [](swiv3d::EntityState& actor, int grant) {
        actor.words[21] = grant; actor.words[20] = 1; actor.words[5] = 11;
        actor.words[22] = actor.words[23] = 3000;
    };
    World repair;
    repair.state.entities[0].words[2] = 450;
    tether(repair.add(2999, swiv3d::Behavior::pickup, 0x441, 10), 1101);
    auto repair_simulation = repair.simulation();
    repair_simulation.advance({});
    require(repair.state.entities[0].words[2] == 496, "Repair grant exceeded normal health");

    World ammo;
    ammo.state.weapons.ammunition[0] = 99;
    tether(ammo.add(2999, swiv3d::Behavior::pickup, 0x441, 10), 1000);
    auto ammo_simulation = ammo.simulation();
    ammo_simulation.advance({});
    require(ammo.state.weapons.ammunition[0] == 100, "Missile grant exceeded its ammunition limit");

    World refill;
    refill.state.entities[0].words[2] = 100;
    tether(refill.add(2999, swiv3d::Behavior::pickup, 0x441, 10), 1107);
    tether(refill.add(2998, swiv3d::Behavior::pickup, 0x441, 10), 1107);
    auto refill_simulation = refill.simulation();
    refill_simulation.advance({});
    require(refill.state.entities[0].words[2] == 696 && refill.state.entities.back().words[2] == 300,
            "Repeated shield grant failed to refill energy and increase health");
}

void test_objective_pickup_collection() {
    constexpr int objective_base = 5000;
    constexpr int objective_priorities = 10;
    for (int priority = 0; priority < objective_priorities; ++priority) {
        World world;
        world.state.weapons.ammunition[0] = 7;
        auto& pickup = world.add(2999,swiv3d::Behavior::pickup,0x2441,100);
        pickup.words[21] = objective_base + priority;
        pickup.words[20] = 1;
        pickup.words[5] = 11;
        pickup.words[22] = pickup.words[23] = 3000;
        world.add(2998,swiv3d::Behavior::pose,0x2001,100);
        world.state.objectives = 2;
        world.state.objective_targets = {{2999,priority},{2998,0}};
        const auto ammunition = world.state.weapons.ammunition;
        auto simulation = world.simulation();
        simulation.advance({});
        require(world.state.entities[1].words[1] == 0 && world.state.weapons.collected == 1,
                "Objective pickup did not finish normal collection");
        require(world.state.objectives_completed == 1 && world.state.objective_targets.size() == 1 &&
                world.state.objective_targets.front().entity == 2998 && world.state.finish_counter == -1,
                "Objective pickup lost its earned objective bookkeeping");
        require(world.state.entities[0].words[2] == 496 && world.state.weapons.ammunition == ammunition &&
                world.state.weapons.primary_level == 0,
                "Objective pickup changed the player loadout");
    }
}

void test_rocket_buddy_pickup() {
    World world;
    auto& pickup = world.add(2999,swiv3d::Behavior::pickup,0x441,10);
    pickup.words[21] = 1105;
    pickup.words[20] = 1;
    pickup.words[5] = 11;
    pickup.words[22] = pickup.words[23] = 3000;
    auto simulation = world.simulation();
    simulation.advance({});
    const int slot = world.state.weapons.buddies[0];
    require(slot != swiv3d::no_entity,"Rocket buddy pickup did not occupy its first formation slot");
    const auto buddy = std::find_if(world.state.entities.begin(),world.state.entities.end(),[slot](const auto& actor) { return actor.slot == slot; });
    require(buddy->words[19] == 851 && buddy->words[2] == 250 && buddy->words[23] == 3000,
            "Rocket buddy lost its original model, health or owner");
    require(buddy->words[7] == world.state.entities.front().words[7] - 16384,"Rocket buddy lost its left formation offset");
    swiv3d::KeyboardState keys{};
    keys[0x39] = true;
    for (int tick = 0; tick < 7; ++tick) { simulation.advance(keys); }
    const auto rockets = std::count_if(world.state.entities.begin(),world.state.entities.end(),[](const auto& actor) {
        return actor.words[19] == 10002 && actor.words[2] == 8 && actor.words[23] == 3000 && actor.words[5] == 30;
    });
    require(rockets == 2,"Rocket buddy did not fire its owned pair on phase seven");
    for (int tick = 0; tick < 25; ++tick) { simulation.advance(keys); }
    std::vector<int> vertical;
    for (const auto& actor : world.state.entities) {
        const auto& w = actor.words;
        if (w[19] == 10002 && w[2] == 8 && w[23] == 3000 && w[5] == 30) {
            require(w[10] == 0 && w[12] == 9000,"Rocket buddy changed its forward firing velocity");
            vertical.push_back(w[11]);
        }
    }
    std::sort(vertical.begin(),vertical.end());
    require(vertical == std::vector<int>{-150,150},"Rocket buddy lost the vertical pair on phase zero");
    std::find_if(world.state.entities.begin(),world.state.entities.end(),[slot](const auto& actor) { return actor.slot == slot; })->words[2] = 0;
    simulation.advance({});
    require(world.state.weapons.buddies[0] == swiv3d::no_entity,"Destroyed rocket buddy retained its formation slot");

}

void test_tracking_buddy_pickup() {
    World world;
    auto& pickup = world.add(2999,swiv3d::Behavior::pickup,0x441,10);
    pickup.words[21] = 1106;
    pickup.words[20] = 1;
    pickup.words[5] = 11;
    pickup.words[22] = pickup.words[23] = 3000;
    auto simulation = world.simulation();
    simulation.advance({});
    const int slot = world.state.weapons.buddies[1];
    require(slot != swiv3d::no_entity,"Tracking buddy did not occupy its first formation slot");
    auto buddy = std::find_if(world.state.entities.begin(),world.state.entities.end(),[slot](const auto& actor) { return actor.slot == slot; });
    require(buddy->words[19] == 852 && buddy->words[2] == 70 && buddy->words[23] == 3000,
            "Tracking buddy lost its original model, health or owner");
    const int x = buddy->words[7], y = buddy->words[8], z = buddy->words[9];
    auto& target = world.add(2997,swiv3d::Behavior::mounted_weapon,0x1001,500);
    target.words[7] = x; target.words[8] = y; target.words[9] = z + 100000;
    auto tracking_simulation = world.simulation();
    buddy = std::find_if(world.state.entities.begin(),world.state.entities.end(),[slot](const auto& actor) { return actor.slot == slot; });
    buddy->words[22] = buddy->slot;
    buddy->words[6] = 11;
    buddy->words[5] = 15;
    buddy->words[13] = buddy->words[14] = 0;
    tracking_simulation.advance({});
    require(world.state.weapons.primary_held == 0,"Tracking buddy test unexpectedly held primary fire");
    const auto bullet = std::find_if(world.state.entities.begin(),world.state.entities.end(),[](const auto& actor) {
        return actor.words[19] == 10000 && actor.words[2] == 14 && actor.words[23] == 3000 && actor.words[5] == 30;
    });
    require(bullet != world.state.entities.end() && bullet->words[10] == 0 && bullet->words[11] == 0 && bullet->words[12] == 9000,
            "Tracking buddy did not independently acquire a ground target and fire its owned projectile");
    tracking_simulation.advance({});
    const auto fresh_bullets = [&] {
        return std::count_if(world.state.entities.begin(),world.state.entities.end(),[](const auto& actor) {
            return actor.words[19] == 10000 && actor.words[2] == 14 && actor.words[23] == 3000 && actor.words[5] == 30;
        });
    };
    require(fresh_bullets() == 0,"Tracking buddy fired outside its autonomous burst phases");
    tracking_simulation.advance({});
    buddy = std::find_if(world.state.entities.begin(),world.state.entities.end(),[slot](const auto& actor) { return actor.slot == slot; });
    buddy->words[8] = y;
    buddy->words[11] = buddy->words[13] = buddy->words[14] = 0;
    tracking_simulation.advance({});
    require(fresh_bullets() == 1,"Tracking buddy lost the second shot on phase three");
    std::find_if(world.state.entities.begin(),world.state.entities.end(),[slot](const auto& actor) { return actor.slot == slot; })->words[2] = 0;
    tracking_simulation.advance({});
    require(world.state.weapons.buddies[1] == swiv3d::no_entity,"Destroyed tracking buddy retained its formation slot");
}

void test_bomber_buddy_pickup() {
    World world;
    auto& pickup = world.add(2999, swiv3d::Behavior::pickup, 0x441, 10);
    pickup.words[21] = 1108;
    pickup.words[20] = 1;
    pickup.words[5] = 11;
    pickup.words[22] = pickup.words[23] = 3000;
    auto simulation = world.simulation();
    simulation.advance({});
    const auto buddy = std::find_if(world.state.entities.begin(), world.state.entities.end(), [](const auto& actor) {
        return actor.words[19] == 810;
    });
    require(buddy != world.state.entities.end() && buddy->words[2] == 70 && buddy->words[23] == 3000,
            "Bomber buddy pickup lost its model, health or parent");
    const int slot = buddy->slot;
    require(world.state.weapons.buddies[1] == slot && buddy->words[7] == world.state.entities[0].words[7] + 16384,
            "Bomber buddy lost its original slot order or formation offset");
    swiv3d::KeyboardState keys{};
    keys[0x39] = true;
    for (int update = 0; update < 32; ++update) { simulation.advance(keys); }
    const auto bomb = std::find_if(world.state.entities.begin(),world.state.entities.end(),[](const auto& actor) {
        return static_cast<swiv3d::Behavior>(actor.words[26]) == swiv3d::Behavior::buddy_bomb;
    });
    require(bomb != world.state.entities.end() && bomb->words[2] == 20 && bomb->words[23] == 3000,
            "Bomber buddy did not launch its owned bomb at the 32-update boundary");
    std::find_if(world.state.entities.begin(),world.state.entities.end(),[slot](const auto& actor) {
        return actor.slot == slot;
    })->words[2] = 0;
    simulation.advance({});
    require(world.state.weapons.buddies[1] == swiv3d::no_entity,"Destroyed bomber buddy retained its occupied slot");
}

void test_colored_primary_families() {
    constexpr int burst_counts[]{3,3,7,7,6,9,9,6,10,10,3,5,6,13,17};
    for (int level = 5; level < 20; ++level) {
        World world;
        world.state.weapons.primary_level = level;
        world.state.flight.control_ticks = 63;
        auto simulation = world.simulation();
        swiv3d::KeyboardState keys{};
        keys[0x39] = true;
        simulation.advance(keys);
        require(world.state.shots == static_cast<unsigned>(burst_counts[level - 5]), "Colored primary burst count changed");
    }
}

void test_player_death_transition() {
    World world;
    world.state.flight.craft = swiv3d::CraftKind::hover;
    world.state.flight.weapon_hold = true;
    world.state.entities[0].words[26] = static_cast<int>(swiv3d::Behavior::hover);
    world.add(2999, swiv3d::Behavior::pose, 0x14, 100).words[22] = world.state.player;
    world.state.weapons.buddies[0] = 2999;
    world.state.entities[0].words[2] = 0;
    auto simulation = world.simulation();
    simulation.advance({});
    require(world.state.entities[0].words[1] == 0x40 && world.state.entities[0].words[19] == 0,
            "Dead player did not become the original invisible dummy craft");
    require(world.state.flight.control == swiv3d::PlayerControl::disabled && world.state.flight.weapon_hold,
            "Player death lost the original disabled control or retained weapon-hold state");
    require(std::all_of(world.state.weapons.buddies.begin(), world.state.weapons.buddies.end(),
                       [](int slot) { return slot == swiv3d::no_entity; }),
            "Player death retained buddy formation references");
    require(std::any_of(world.state.sound_events.begin(), world.state.sound_events.end(), [](const auto& event) {
        return event.sound == 5002 && event.action == swiv3d::SoundAction::stop;
    }), "Hover death did not stop its engine sound");
    for (int i = 0; i < 35; ++i) { simulation.advance({}); }
    require(world.state.tick == 36 && world.state.phase == swiv3d::MissionPhase::failed && world.state.finish_counter == 35,
            "Death transition did not retain its 35-update delay");
    World fatal_burst;
    fatal_burst.state.entities[0].words[2] = 0;
    fatal_burst.state.flight.control_ticks = 2;
    auto fatal_simulation = fatal_burst.simulation();
    swiv3d::KeyboardState keys{}; keys[0x39] = true;
    fatal_simulation.advance(keys);
    require(fatal_burst.state.shots == 2 && fatal_burst.state.flight.control_ticks == 3,
            "Fatal update skipped controls before GAME_kill");
}

void test_completion_priority_and_extraction() {
    World world;
    world.add(2999, swiv3d::Behavior::pose, 1, 0);
    world.state.objective_targets = {{2999,0}};
    world.state.objectives = 1;
    auto simulation = world.simulation();
    simulation.advance({});
    require(world.state.finish == swiv3d::FinishKind::success && world.state.objectives_completed == 1,
            "Last required objective did not start mission completion");
    for (int i = 0; i < 24; ++i) { simulation.advance({}); }
    require(world.state.extracting && world.state.entities[0].words[1] == 0x40,
            "Mission extraction did not begin at finish update 25");
    for (int i = 0; i < 30; ++i) { simulation.advance({}); }
    require(world.state.phase == swiv3d::MissionPhase::completed, "Mission did not complete at finish update 55");
}

void test_completion_message_during_pending_finish() {
    World world;
    world.add(2999,swiv3d::Behavior::pose,1,0);
    world.state.objective_targets = {{2999,0}};
    world.state.objectives = 1;
    world.state.finish = swiv3d::FinishKind::success;
    world.state.finish_counter = 1;
    auto simulation = world.simulation();
    simulation.advance({});
    require(world.state.objectives_completed == 1 && world.state.messages.empty(),
            "Pending completion emitted another objective message");
}

void test_special_families() {
    for (int slot = 2; slot < 19; ++slot) {
        if (slot == 9) { continue; }
        World world;
        world.state.weapons.ammunition[slot] = 3;
        world.state.flight.bindings[7] = 0x32;
        auto simulation = world.simulation();
        for (int i = 0; i < 70; ++i) { simulation.advance({}); }
        require(world.state.weapons.selected == slot && world.state.weapons.phase == 10,
                "Special family did not deploy to ready");
        swiv3d::KeyboardState keys{}; keys[0x32] = true;
        simulation.advance(keys);
        require(world.state.weapons.ammunition[slot] == 2, "Special family consumed the wrong ammunition count");
        for (int i = 0; i < 170; ++i) { simulation.advance({}); }
        require(world.state.weapons.ammunition[slot] == 2, "Special family consumed ammo again during its release sequence");
        world.state.weapons.ammunition[0] = 6;
        keys.fill(false); keys[2] = true;
        simulation.advance(keys);
        for (int i = 0; i < 100; ++i) { simulation.advance({}); }
        require(world.state.weapons.selected == 0 && world.state.weapons.phase == 10,
                "Special family did not holster before switching back to a single missile");
    }
}

void test_enemy_beam_and_direct_fire() {
    for (int gun : {600, 603, 700, 701}) {
        World world;
        auto& source = world.add(2999, swiv3d::Behavior::pillbox, 0x1001, 100);
        source.words[5] = gun >= 700 ? 25 : 1;
        source.words[9] += 20000;
        source.words[21] = gun;
        source.words[22] = world.state.player;
        world.state.entities[0].words[1] |= 0x80000;
        auto simulation = world.simulation();
        simulation.advance({});
        if (gun >= 700) {
            require(world.state.entities[0].words[2] == (gun == 700 ? 485 : 477), "Enemy beam did not damage its target");
        } else {
            require(world.state.shots == 1, "Direct enemy gun did not emit a projectile");
        }
    }
}

void test_boat_water_node_and_surface() {
    World world;
    world.state.entities[0].words[1] |= 0x80000;
    auto& boat = world.add(2999, swiv3d::Behavior::boat, 0x1003, 100);
    boat.words[8] = 0;
    boat.words[6] = 41;
    boat.words[0] = swiv3d::no_entity;
    world.state.entities[0].words[7] += 2 * 131072;
    world.state.entities[0].words[0] = swiv3d::no_entity;
    world.state.grid[2] = world.state.player;
    const auto resources = [](std::uint32_t id) {
        if (id != 107) { return std::vector<std::uint8_t>(32); }
        std::vector<std::uint8_t> nodes(72);
        const auto put = [&](int offset, int value) {
            for (int i = 0; i < 4; ++i) { nodes[offset + i] = static_cast<std::uint8_t>(static_cast<unsigned>(value) >> (i * 8)); }
        };
        for (int offset : {0,4,8,12}) { put(offset, -1); }
        put(16, 1000); put(20, 64); put(24, 1); put(36 + 24, -1);
        return nodes;
    };
    swiv3d::detail::Combat simulation(world.state, [](int, int) { return 0; }, resources, {},
                                     [](int, int) { return 1000; });
    simulation.advance({});
    require(world.state.entities[1].words[16] == 0, "Boat did not reserve a water node when land capacity was zero");
    require(world.state.entities[1].words[8] == 1000 && world.state.entities[1].words[11] == 0,
            "Boat hull contacted terrain instead of the water surface");
    require(world.state.entities[1].words[22] == world.state.player,
            "Water patrol failed to acquire a target two cells away");
}

void test_strafe_factory_launch() {
    World world;
    world.state.entities[0].words[1] |= 0x80000;
    auto& factory = world.add(2999, swiv3d::Behavior::strafe_factory, 1, 100);
    factory.words[6] = 91; factory.words[22] = world.state.player;
    const auto resources = [](std::uint32_t id) {
        std::vector<std::uint8_t> data(64);
        if (id == 105) {
            data[11 * 4] = 101; data[13 * 4] = 50; data[14 * 4] = 100; data[15 * 4] = 50;
        }
        return data;
    };
    swiv3d::detail::Combat simulation(world.state, [](int, int) { return 0; }, resources);
    simulation.advance({});
    const auto plane = std::find_if(world.state.entities.begin(), world.state.entities.end(), [](const auto& actor) {
        return static_cast<swiv3d::Behavior>(actor.words[26]) == swiv3d::Behavior::strafe_plane;
    });
    require(plane != world.state.entities.end(), "Strafing factory retained the attack-plane launch delay");
    require(plane->words[19] == 101 && plane->words[2] == 50 && plane->words[21] == 100 &&
            plane->words[22] == 3000 && world.state.entities[1].words[6] == 1,
            "Factory did not preserve launch parameters, target or counter order");
}

void test_vehicle_return_turn_cache() {
    World world;
    auto& station = world.add(2999,swiv3d::Behavior::none,1,1000);
    station.words[7] += 1409; station.words[9] += 40525;
    auto& player = world.state.entities.front().words;
    const int x = player[7], z = player[9];
    player[1] = 0x40; player[13] = 3; player[20] = 31; player[23] = 2999;
    player[26] = static_cast<int>(swiv3d::Behavior::buggy_change);
    world.state.flight.control = swiv3d::PlayerControl::disabled;
    world.state.flight.weapon_hold = true;
    world.state.direction_steps = {-6,-3,0,3,6};
    auto simulation = world.simulation();
    simulation.advance({});
    const auto& moved = world.state.entities.front().words;
    require(moved[13] == 5 && moved[7] == x + 64 && moved[9] == z + 1024,
            "Vehicle return scored the old positive turn before updating the shared cache");
}

void test_conditional_pickup_gate() {
    World world;
    world.state.entities[0].words[1] |= 0x80000;
    world.add(2999,swiv3d::Behavior::conditional_pickup,0x841,10);
    const auto resources = [](std::uint32_t id) {
        std::vector<std::uint8_t> data(64);
        if (id == 105) { data[16] = 101; data[20] = 102; data[24] = 10; data[28] = 0xe8; data[29] = 3; }
        return data;
    };
    swiv3d::detail::Combat simulation(world.state,[](int,int) { return 0; },resources);
    simulation.advance({});
    require(world.state.entities.size() == 2,"Conditional pickup ignored the objective gate");
    world.state.last_objective = 9;
    simulation.advance({});
    const auto count = [&] { return std::count_if(world.state.entities.begin(),world.state.entities.end(),[](const auto& actor) {
        return static_cast<swiv3d::Behavior>(actor.words[26]) == swiv3d::Behavior::pickup;
    }); };
    require(count() == 1 && world.state.entities.back().words[21] == 1000,
            "Conditional pickup did not reveal the command's grant");
    require(world.state.entities.back().words[19] == 102,"Conditional pickup used its hidden object's model");
    simulation.advance({});
    require(count() == 1,"Conditional pickup spawned twice");
}

void test_intruder_without_patrol_node() {
    World world;
    world.state.entities.front().words[1] |= 0x80000;
    world.add(2998,swiv3d::Behavior::none,1,100).words[21] = 1018;
    auto& factory = world.add(2995,swiv3d::Behavior::none,0x40,100);
    factory.words[22] = 3000;
    auto& aircraft = world.add(2994,swiv3d::Behavior::intruder,0x1033,0);
    aircraft.words[23] = 2995; aircraft.words[5] = 100; aircraft.words[20] = 1;
    world.state.weapons.mounts[8] = 2994;
    auto simulation = world.simulation();
    simulation.advance({});
    const auto& dead = world.state.entities[3].words;
    require(static_cast<swiv3d::Behavior>(dead[26]) == swiv3d::Behavior::dead_aircraft &&
            world.state.unassigned_air_returns == 1 && world.state.weapons.mounts[8] == 2994 &&
            world.state.entities[1].words[21] == 0,
            "Intruder death lost its unassigned-node or extended parent-slot alias");
}

void test_analog_ground_controls() {
    for (const auto craft : {swiv3d::CraftKind::buggy,swiv3d::CraftKind::ski}) {
        World world;
        world.state.entities.front().words[1] |= 0x80000;
        auto& flight = world.state.flight;
        flight.craft = craft;
        flight.controller = swiv3d::ControllerKind::joystick;
        flight.axes = {-128,128,0};
        for (int i = 0; i < 4; ++i) {
            world.add(2999-i,swiv3d::Behavior::wheel,0x80040,0);
            world.state.weapons.mounts[3+i] = 2999-i;
        }
        auto simulation = world.simulation();
        simulation.advance({});
        require(flight.power == 130 && flight.wheel_target == -50 && world.state.entities.front().words[13] == -5,
                "Ground analog controls lost original acceleration, steering scale or signed shift");
        if (craft == swiv3d::CraftKind::buggy) {
            require(world.state.entities[3].words[14] == -15 && world.state.entities[4].words[14] == 0,
                    "Analog buggy input changed the original single-wheel rotation update");
        }
        flight.axes = {0,-1,0};
        simulation.advance({});
        require(flight.power == 128,"Negative fractional ground acceleration rounded toward zero");
    }
}

void test_ground_and_hover_mounts() {
    for (const auto craft : {swiv3d::CraftKind::buggy,swiv3d::CraftKind::ski,swiv3d::CraftKind::hover}) {
        World world;
        world.state.entities.front().words[1] |= 0x80000;
        world.state.flight.craft = craft;
        world.state.flight.control = swiv3d::PlayerControl::disabled;
        world.state.weapons.selected = world.state.weapons.requested = 1;
        world.state.weapons.ammunition[1] = 3;
        auto simulation = world.simulation();
        for (int tick = 0; tick < 42; ++tick) { simulation.advance({}); }
        const auto& player = world.state.entities.front().words;
        const int slot = world.state.weapons.mounts[0];
        const auto& mount = std::find_if(world.state.entities.begin(),world.state.entities.end(),
            [&](const auto& actor) { return actor.slot == slot; })->words;
        const bool hover = craft == swiv3d::CraftKind::hover;
        require(mount[7] - player[7] == (hover ? 8000 : 5000) && mount[8] - player[8] == (hover ? -700 : 2000),
                "Special weapon lost its ground/hover mounting geometry");
    }
}

void test_booster_and_glider() {
    for (const int slot : {9,19}) {
        World world;
        world.state.entities.front().words[1] |= 0x80000;
        world.state.flight.craft = swiv3d::CraftKind::buggy;
        world.state.flight.control = swiv3d::PlayerControl::disabled;
        auto& weapon = world.state.weapons;
        weapon.selected = weapon.requested = slot; weapon.ammunition[slot] = 2;
        auto simulation = world.simulation();
        for (int tick = 0; tick < 22; ++tick) { simulation.advance({}); }
        require(weapon.phase == 10 && weapon.mounts[0] != swiv3d::no_entity,"Ground launcher did not deploy");
        weapon.phase = 11;
        simulation.advance({});
        require(weapon.ammunition[slot] == 1,"Ground special did not consume ammunition");
        if (slot == 9) {
            require(weapon.phase == 12 && world.state.flight.control == swiv3d::PlayerControl::ground_transition &&
                    (world.state.entities.front().words[1] & 4) == 0,"Booster did not transfer control");
        } else {
            const auto& player = world.state.entities.front().words;
            require(weapon.phase == 10 && player[11] == -60 && player[12] == 448,"Glider did not apply thrust");
        }
    }
}

void test_airborne_reacquisition_delay() {
    World world;
    auto& aircraft = world.add(2999,swiv3d::Behavior::aircraft,0x1033,100);
    aircraft.words[7] += 32768; aircraft.words[6] = 21;
    auto simulation = world.simulation();
    simulation.advance({});
    const auto& w = world.state.entities[1].words;
    require(w[22] == 3000 && w[13] == 0 && w[14] == 0,
            "Airborne reacquisition steered before the next actor update");
}

void test_ground_second_pod_alias() {
    World world;
    world.state.entities.front().words[1] |= 0x80000;
    world.state.flight.craft = swiv3d::CraftKind::buggy;
    world.state.flight.control = swiv3d::PlayerControl::disabled;
    world.add(2999,swiv3d::Behavior::mounted_weapon,0x40,100).words[23] = 3000;
    world.add(2998,swiv3d::Behavior::mounted_weapon,0x40,100).words[23] = 3000;
    auto& weapon = world.state.weapons;
    weapon.selected = weapon.requested = 2; weapon.ammunition[2] = 2;
    weapon.mounts[0] = 2999; weapon.mounts[1] = 2998;
    weapon.phase = 12; weapon.progress = 84;
    auto simulation = world.simulation();
    simulation.advance({});
    const auto& first = world.state.entities[1].words;
    const auto& second = world.state.entities[2].words;
    require(first[10] == -400 && first[11] == 900 && first[12] == -300 && first[20] == 1 &&
            second[10] == 0 && second[11] == 0 && second[12] == 0 && second[20] == 0 &&
            static_cast<swiv3d::Behavior>(second[26]) == swiv3d::Behavior::falling_casing,
            "Ground second pod did not preserve the original first-pod kick alias");
}

void test_ground_extraction_controls() {
    for (const auto craft : {swiv3d::CraftKind::buggy,swiv3d::CraftKind::ski}) {
        World world;
        const auto height = [](int,int) { return 0; };
        const auto resource = [](std::uint32_t) { return std::vector<std::uint8_t>(32); };
        swiv3d::detail::Combat simulation(world.state,height,resource,{},height,craft);
        world.state.entities.front().words[1] |= 0x80000;
        world.state.finish = swiv3d::FinishKind::success;
        world.state.finish_counter = 25; world.state.extracting = true;
        simulation.advance({});
        require(world.state.flight.power == 130 && world.state.entities.front().words[13] == -4 &&
                world.state.flight.control_ticks == 0,"Ground extraction used the flight controller or advanced its firing phase");
        for (int tick = 0; tick < 29; ++tick) { simulation.advance({}); }
        require(world.state.phase == swiv3d::MissionPhase::completed,"Ground extraction lost the completion boundary");
    }
}

void test_coasting_missile_trail_interval() {
    World world;
    world.state.entities.front().words[1] |= 0x80000;
    auto& missile = world.add(2999,swiv3d::Behavior::missile_coast,0x3f,100);
    missile.words[23] = 3000; missile.words[6] = 7;
    auto simulation = world.simulation();
    simulation.advance({});
    require(world.state.entities.size() == 2,"Coasting missile emitted its trail before age eight");
    simulation.advance({});
    require(world.state.entities.size() == 3 && world.state.entities.back().words[20] == 13,
            "Coasting missile lost its eight-update trail cadence");
}
void test_ground_exit_delay() {
    World world;
    world.state.flight.craft = swiv3d::CraftKind::buggy;
    world.state.entities[0].words[1] = 0x207;
    world.state.entities[0].words[26] = static_cast<int>(swiv3d::Behavior::none);
    auto& exit = world.add(2999,swiv3d::Behavior::ground_exit,1,1000);
    exit.words[20] = 5;
    world.state.objectives = 1; world.state.objective_targets.push_back({2999,0});
    for (int i = 0; i < 4; ++i) {
        world.add(2998 - i,swiv3d::Behavior::wheel,0x40,0);
        world.state.weapons.mounts[3 + i] = 2998 - i;
    }
    swiv3d::detail::Combat simulation(world.state,[](int,int) { return 0; },[](std::uint32_t id) {
        if (id != 107) { return std::vector<std::uint8_t>(32); }
        std::vector<std::uint8_t> nodes(72);
        nodes[16] = nodes[20] = 64; nodes[32] = 1;
        std::fill_n(nodes.begin() + 60,4,255);
        return nodes;
    });
    simulation.advance({});
    require(world.state.finish == swiv3d::FinishKind::delayed_success && world.state.finish_counter == 81 &&
            world.state.phase == swiv3d::MissionPhase::playing && !world.state.extracting,
            "Ground exit used helicopter extraction instead of its delayed completion counter");
    require(world.state.flight.control == swiv3d::PlayerControl::ground_transition && world.state.flight.weapon_hold &&
            static_cast<swiv3d::Behavior>(world.state.entities[0].words[26]) == swiv3d::Behavior::buggy_exit,
            "Ground exit did not retain the automatic node-following controller");
}

void test_teleport_retains_departure_region() {
    World world;
    world.state.entities[0].words[26] = static_cast<int>(swiv3d::Behavior::none);
    auto& source = world.add(2999,swiv3d::Behavior::teleport,0x841,100);
    source.words[20] = 10; source.words[21] = 7000;
    auto& destination = world.add(2998,swiv3d::Behavior::teleport,0x841,100);
    destination.words[21] = 7000; destination.words[7] += 20 * 131072;
    world.add(2997,swiv3d::Behavior::none,0x40,100);
    world.state.grid.fill(swiv3d::no_entity);
    for (auto& actor : world.state.entities) {
        const auto cell = ((actor.words[7] >> 17) & 63) + ((actor.words[9] >> 11) & 4032);
        actor.words[0] = world.state.grid[cell]; world.state.grid[cell] = actor.slot;
    }
    auto simulation = world.simulation();
    simulation.advance({});
    require(world.state.camera_jump,"Teleport did not invalidate the retained camera");
    require(world.state.entities[0].words[7] == world.state.entities[2].words[7],"Teleport did not move the player to its paired destination");
    const auto& effects = world.state.ground_effects;
    require(effects.active == 4 && effects.records[4].kind == swiv3d::GroundEffectKind::teleport &&
            effects.records[4].lifetime == 39 && effects.records[4].position[0] == world.state.entities[0].words[7],
            "Teleport did not retain its spatial lighting record through the first update");
    simulation.advance({});
    require(world.state.entities[3].words[6] == 2,"Teleport discarded the departure region before its final full update");
    simulation.advance({}); simulation.advance({});
    require(world.state.entities[3].words[6] == 2,"Teleport kept executing the departed region after its two-pass handoff");
}

void test_scatter_explosion_start() {
    World world;
    world.state.entities.front().words[1] |= 0x80000;
    auto& missile = world.add(2999,swiv3d::Behavior::scatter_missile,0x3f,80);
    missile.words[23] = 3000; missile.words[5] = 34;
    auto simulation = world.simulation();
    simulation.advance({});
    const auto explosion = std::find_if(world.state.entities.begin(),world.state.entities.end(),[](const auto& actor) {
        return static_cast<swiv3d::Behavior>(actor.words[26]) == swiv3d::Behavior::aircraft_explosion;
    });
    require(explosion != world.state.entities.end() && explosion->words[20] == 13,
            "Scatter burst did not start at the original explosion sequence's first frame");
}

void test_air_node_search_horizon() {
    World world;
    world.state.entities[0].words[1] |= 0x80000;
    auto& shot = world.add(2999,swiv3d::Behavior::pyramid_shot,0x18,50);
    shot.words[7] = 74 * 256; shot.words[8] = 32000; shot.words[22] = 3000;
    std::vector<std::uint8_t> nodes(8 * 36);
    const auto put = [&](int node,int field,int value) {
        for (int byte = 0; byte < 4; ++byte) {
            nodes[node * 36 + field * 4 + byte] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(value) >> (byte * 8));
        }
    };
    for (int node = 0; node < 7; ++node) {
        for (int edge = 0; edge < 4; ++edge) { put(node,edge,-1); }
        if (node < 6) { put(node,0,node + 1); }
        put(node,4,node == 0 ? 74 : node == 6 ? 64 : 164 + node);
        put(node,5,64); put(node,7,1);
    }
    put(7,6,-1);
    swiv3d::detail::Combat simulation(world.state,[](int,int) { return 0; },[&](std::uint32_t id) {
        return id == 107 ? nodes : std::vector<std::uint8_t>(32);
    });
    simulation.advance({});
    require(world.state.entities[1].words[16] == 0 &&
            static_cast<swiv3d::Behavior>(world.state.entities[1].words[26]) == swiv3d::Behavior::pyramid_dive,
            "Air node search looked past its five-edge horizon instead of committing to the dive");
}

void test_hover_ground_impact_effects() {
    World world;
    world.state.flight.craft = swiv3d::CraftKind::hover;
    world.state.entities[0].words[26] = static_cast<int>(swiv3d::Behavior::hover);
    world.state.entities[0].words[8] = 0;
    auto simulation = world.simulation();
    simulation.advance({});
    require(world.state.flight.ground_impacts == 1 && world.state.effects_phase == 1,
            "Hover ground impact omitted its smoke jitter step");
    require(world.state.entities.size() == 3 &&
            static_cast<swiv3d::Behavior>(world.state.entities[1].words[26]) == swiv3d::Behavior::explosion &&
            world.state.entities[1].words[20] == 27 &&
            static_cast<swiv3d::Behavior>(world.state.entities[2].words[26]) == swiv3d::Behavior::smoke,
            "Hover ground impact omitted its explosion and smoke actors");
}

void test_drone_unmasked_heading() {
    World world;
    world.state.entities[0].words[1] |= 0x80000;
    auto& drone = world.add(2999,swiv3d::Behavior::drone,0x1001,160);
    drone.words[3] = 50; drone.words[13] = -10; drone.words[22] = 3000;
    swiv3d::detail::Combat simulation(world.state,[](int,int) { return 0; },[](std::uint32_t id) {
        if (id != 107) { return std::vector<std::uint8_t>(32); }
        std::vector<std::uint8_t> nodes(72);
        std::fill_n(nodes.begin(),16,255);
        nodes[16] = 64; nodes[20] = 232; nodes[21] = 3; nodes[28] = 1;
        std::fill_n(nodes.begin() + 60,4,255);
        return nodes;
    });
    simulation.advance({});
    require(world.state.entities[1].words[10] == 3 && world.state.entities[1].words[12] == -8,
            "Drone wrapped its transient negative heading instead of indexing the adjacent original tables");
}

void test_ground_cruise_age_alias() {
    World world;
    world.state.entities[0].words[1] |= 0x80000;
    world.state.flight.craft = swiv3d::CraftKind::buggy;
    world.state.flight.control = swiv3d::PlayerControl::disabled;
    world.add(2999,swiv3d::Behavior::none,0x40,100).words[6] = 7;
    world.add(2998,swiv3d::Behavior::mounted_weapon,0x40,100).words[6] = 41;
    auto& weapon = world.state.weapons;
    weapon.selected = weapon.requested = 13; weapon.ammunition[13] = 2; weapon.phase = 13;
    weapon.mounts[0] = 2999; weapon.mounts[1] = 2998;
    auto simulation = world.simulation();
    simulation.advance({});
    require(world.state.entities[1].words[6] == 0 && world.state.entities[2].words[6] == 42,
            "Ground cruise launch reset the second missile instead of the original first-missile age alias");
}

void test_offscreen_node_release() {
    World world;
    auto& aircraft = world.add(2999,swiv3d::Behavior::air_patrol,0x1033,100);
    aircraft.words[7] = 5 * 131072;
    aircraft.words[0] = swiv3d::no_entity;
    world.state.grid[0] = 3000; world.state.grid[5] = 2999;
    swiv3d::detail::Combat simulation(world.state,[](int,int) { return 0; },[](std::uint32_t id) {
        if (id != 107) { return std::vector<std::uint8_t>(32); }
        std::vector<std::uint8_t> nodes(72);
        nodes[16] = nodes[20] = 64; nodes[28] = 1;
        std::fill_n(nodes.begin() + 60,4,255);
        return nodes;
    });
    simulation.advance({});
    require(world.state.entities[1].words[1] == 0 && world.state.entities[1].words[16] == 0 &&
            world.state.entities[1].words[6] == 0,
            "Offscreen retirement skipped the aircraft's node-release callback");
}

}

int main() {
    try {
        test_direction_scratch_alias();
        test_ski_water_contacts();
        test_ground_engine_scratch_alias();
        test_changeover_scan_counter();
        test_message_lifetime();
        test_ground_exit_delay();
        test_teleport_retains_departure_region();
        test_scatter_explosion_start();
        test_air_node_search_horizon();
        test_hover_ground_impact_effects();
        test_drone_unmasked_heading();
        test_ground_cruise_age_alias();
        test_offscreen_node_release();
        test_primary_bursts_and_expiration();
        test_damage_and_owner_exclusion();
        test_turret_acquisition_and_return_fire();
        test_aim_clamp_and_idle_tank_top();
        test_pool_reuses_canonical_slots();
        test_pickup_collection_and_special();
        test_gun_upgrade_and_full_health_pickup();
        test_upgraded_burst_and_paired_release();
        test_selection_direction_and_shield();
        test_grant_limits_and_shield_refill();
        test_objective_pickup_collection();
        test_rocket_buddy_pickup();
        test_tracking_buddy_pickup();
        test_bomber_buddy_pickup();
        test_colored_primary_families();
        test_player_death_transition();
        test_completion_priority_and_extraction();
        test_completion_message_during_pending_finish();
        test_special_families();
        test_enemy_beam_and_direct_fire();
        test_boat_water_node_and_surface();
        test_strafe_factory_launch();
        test_vehicle_return_turn_cache();
        test_conditional_pickup_gate();
        test_intruder_without_patrol_node();
        test_ground_and_hover_mounts();
        test_analog_ground_controls();
        test_booster_and_glider();
        test_airborne_reacquisition_delay();
        test_ground_second_pod_alias();
        test_ground_extraction_controls();
        test_coasting_missile_trail_interval();
        std::cout << "Combat, pickups, upgrades, missiles, selection and shield tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
