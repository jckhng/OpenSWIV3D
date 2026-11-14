#include "combat.hpp"
#include "combat_math.hpp"

#include <stdexcept>

namespace swiv3d::detail {
namespace {

using namespace combat_math;
enum WeaponPhase { deploy = 0, reload = 1, extending = 2, ready = 10, release = 11,
                   firing = 12, second_release = 13, holster = 20, retracting = 22, change = 30 };
enum class MountPose { moving, ready };
struct Launcher { int model, health, count, first, extension; };
constexpr Launcher launchers[]{
    {800,80,1,0,20}, {801,100,2,0,40}, {850,100,2,0,60}, {856,100,2,0,40},
    {854,80,1,0,40}, {855,80,1,0,40}, {811,30,1,0,40}, {812,80,1,0,40}, {858,80,1,0,40}, {981,80,1,0,20},
    {800,70,3,11,40}, {801,100,4,11,40}, {850,100,2,0,60}, {856,100,2,0,40},
    {854,80,1,0,40}, {855,80,1,0,40}, {811,30,1,0,40}, {812,80,1,0,40}, {858,80,1,0,40}, {813,80,1,0,20}
};

}

void Combat::special_family() {
    auto& weapon = state_.weapons;
    const int slot = weapon.selected;
    const auto& launcher = launchers[slot];
    if (launcher.count == 0) { weapon.phase = change; return; }
    auto& player = entity(state_.player);
    const bool ground = state_.flight.craft == CraftKind::buggy || state_.flight.craft == CraftKind::ski;
    const bool hover = state_.flight.craft == CraftKind::hover;
    const int family = slot % 10;
    if (family == 9 && !ground) { weapon.phase = change; return; }
    const auto mounted = [&](int i) -> EntityState& { return entity(weapon.mounts[launcher.first + i]); };
    const auto position = [&](int i, int progress, MountPose pose) {
        const bool fixed = pose == MountPose::ready;
        int x = 0, y = 0, z = 0;
        if (family == 0 && slot == 0) { y = -150 * progress; }
        else if (family == 0) {
            x = i == 0 ? (hover ? 200 : 100) * progress : i == 1 ? -(hover ? 200 : 100) * progress : 0;
            y = ground ? (i == 2 ? 100 * progress : fixed ? 700 : 17 * progress) :
                hover ? (fixed ? (i == 2 ? 4000 : 700) : 17 * progress) :
                i == 2 ? -100 * progress : fixed ? -700 : -17 * progress;
        } else if (family == 1) {
            x = (i % 2 == 0 ? 1 : -1) * ((ground ? 125 : hover ? 200 : 100) * progress + (slot == 11 && i < 2 ? 3000 : 0));
            y = ground ? 50 * progress : fixed ? -700 : -17 * progress;
        } else if (family == 2) {
            const bool ground_retract_bug = ground && slot == 12 && weapon.phase == retracting;
            const int side = ground_retract_bug ? 100 : ground ? 103 : hover ? 175 : 100;
            x = (i == 0 ? side : -side) * progress;
            y = ground_retract_bug ? -23 * progress : ground ? (fixed ? 2300 : (slot == 12 ? 65 : 35) * progress) :
                fixed ? (hover && slot == 2 && weapon.phase != firing ? -2800 : -1400) :
                (hover && slot == 2 ? -46 : -23) * progress;
            if (fixed && ground) { x = i == 0 ? 6200 : -6200; }
            z = -1500;
        } else if (family == 3) {
            const int side = ground ? 225 : hover ? 300 : 200;
            x = (i == 0 ? side : -side) * progress; y = ground ? 50 * progress : fixed ? -700 : -17 * progress;
        } else if (family == 4) { y = ground ? 100 * progress : fixed ? -3500 : -87 * progress; }
        else if (family == 9) { y = 200 * progress; z = -200 * progress; }
        else {
            const int side = slot == 8 || slot == 15 || slot == 16 || slot == 17 ? -1 : 1;
            x = side * 75 * progress; y = (ground ? 60 : 10) * progress; z = 100 * progress;
        }
        place(player, mounted(i), x, y, z);
    };
    const auto launch = [&](int i, Behavior behavior) {
        auto& w = mounted(i).words;
        w[26] = static_cast<int>(behavior); w[1] = 0x3f; w[22] = player.words[22];
        sound(mounted(i), 2000);
        ++state_.shots;
    };
    const auto discard = [&](int i) {
        auto& object = mounted(i);
        object.words[26] = static_cast<int>(Behavior::falling_casing);
        // Ground multi-launcher two retires its own pod but kicks pod one again.
        auto& kicked = mounted(ground && slot == 2 ? 0 : i);
        const auto velocity = rotate(kicked, ground && slot == 2 ? std::array<int,3>{{i == 0 ? 400 : -400,900,-300}} :
            ground && family == 4 ? std::array<int,3>{{200,1000,-400}} :
            std::array<int,3>{{i == 0 ? 200 : -200,0,slot == 12 ? 4000 : -400}});
        std::copy(velocity.begin(), velocity.end(), kicked.words.begin() + 10);
        kicked.words[20] = i;
        if (ground || hover) { sound(object,3002); }
    };
    const auto drop_booster = [&] {
        auto& casing = mounted(0);
        casing.words[26] = static_cast<int>(Behavior::falling_casing);
        const auto velocity = rotate(casing,{{-100,900,600}});
        std::copy(velocity.begin(),velocity.end(),casing.words.begin() + 10);
        casing.words[20] = 1;
        sound(casing,3002);
    };
    switch (weapon.phase) {
    case deploy:
    case reload:
        {
        const bool deploy_sound = weapon.phase == deploy || (!ground && !hover);
        if (family == 2 && weapon.phase == reload) { break; }
        for (int i = 0; i < launcher.count; ++i) {
            weapon.mounts[launcher.first + i] = spawn(player, 0x40, launcher.health, launcher.model, Behavior::mounted_weapon).slot;
        }
        weapon.progress = 0; weapon.phase = extending;
        if (deploy_sound) { sound(player, 3000); }
        break;
        }
    case extending:
        ++weapon.progress;
        for (int i = 0; i < launcher.count; ++i) { position(i, weapon.progress, MountPose::moving); }
        if (family == 4) { ++mounted(0).words[5]; }
        if (weapon.progress == launcher.extension) { weapon.phase = ready; }
        break;
    case ready:
        for (int i = 0; i < launcher.count; ++i) { position(i, launcher.extension, MountPose::ready); }
        break;
    case release:
        if (family == 9) {
            if (slot == 9) {
                weapon.phase = firing; weapon.progress = 0;
                booster_control_ = state_.flight.control;
                state_.flight.control = PlayerControl::ground_transition;
                player.words[1] &= 0xffffb;
            } else {
                position(0,launcher.extension,MountPose::ready);
                player.words[11] -= 60;
                player.words[10] = shift((player.words[10] + sine(player.words[13]) * 2) * 14,4);
                player.words[12] = shift((player.words[12] + cosine(player.words[13]) * 2) * 14,4);
                trail(mounted(0),0);
                weapon.phase = weapon.ammunition[slot] > 0 ? ready : holster;
            }
            break;
        }
        if (family == 2) { weapon.phase = firing; weapon.progress = 0; break; }
        if (family == 0 || family == 1 || family == 3) {
            const int first = slot == 10 ? 2 : 0;
            launch(first, family == 3 ? Behavior::cruise_missile : Behavior::missile_pair);
            if (family == 3) {
                auto& w = mounted(first).words;
                w[6] = 0; w[5] = 300; w[20] = slot == 3 ? 10 : 20;
            }
            weapon.phase = firing;
            weapon.progress = slot == 10 ? 10 : slot == 11 ? 0 : 7;
            break;
        }
        if (family == 4) {
            auto& pod = mounted(0);
            const int count = slot == 4 ? 5 : 8;
            for (int i = 0; i < count; ++i) {
                pod.words[13] += slot == 4 ? 102 : 64;
                if (ground || hover) { align_attitude(pod,player); }
                else { pod.words[14] = (pod.words[14] + 256) % 512 - 256; }
                auto& missile = spawn(pod, 0x3f, slot == 4 ? 60 : 80, slot == 4 || ground || hover ? 800 : 801, Behavior::missile_pair);
                weapon.mounts[1] = missile.slot;
                // The immediate search uses this missile's heading and the canonical grid.
                search_weapon_target(missile);
                if (missile.words[22] == missile.slot) { missile.words[22] = player.words[22]; }
                weapon.mounts[1] = no_entity;
                ++state_.shots;
            }
            sound(mounted(0), 2000);
            discard(0); weapon.phase = reload; break;
        }
        {
            const auto behavior = family == 5 ? (slot == 5 ? Behavior::scatter_missile : Behavior::cluster_missile) :
                family == 6 ? Behavior::shard_bomb : family == 7 ? (slot == 7 ? Behavior::pulse_ring : Behavior::pulse_wall) :
                slot == 8 ? Behavior::bomber_missile : Behavior::carpet_missile;
            auto& object = mounted(0);
            if (family == 6) {
                object.words[26] = static_cast<int>(behavior);
                const auto velocity = rotate(object, {{0,1500,1300}});
                std::copy(velocity.begin(), velocity.end(), object.words.begin() + 10);
                object.words[20] = slot == 6 ? 0 : 1;
                object.words[14] = 0; object.words[15] = 0; object.words[22] = player.words[22];
                object.words[5] = slot == 6 ? 0 : 2;
            } else {
                object.words[26] = static_cast<int>(behavior); object.words[1] = 0x3f;
                ++state_.shots;
                object.words[15] = 0; object.words[14] = family == 8 ? 60 : 70; object.words[5] = 0;
            }
            object.words[6] = 0; weapon.phase = reload;
            sound(object, 2000);
        }
        break;
    case firing:
        if (slot == 9) {
            ++weapon.progress;
            auto& w = player.words;
            if (weapon.progress < 30) {
                if (w[14] < 90) { w[14] += 4; }
                w[10] = w[12] = 0; w[20] = 2;
                if (weapon.progress == 26) { sound(mounted(0),4020); }
            } else if (weapon.progress < 80) {
                w[20] = 3;
                const auto velocity = rotate(player,{{0,0,9000}});
                std::copy(velocity.begin(),velocity.end(),w.begin() + 10);
                trail(mounted(0),0);
            }
            if (weapon.progress < 73) { position(0,launcher.extension,MountPose::ready); }
            if (weapon.progress == 73) { drop_booster(); }
            if (w[20] == 0) {
                weapon.phase = deploy;
                state_.flight.control = booster_control_;
                w[1] |= 4;
                if (weapon.progress <= 85) {
                    w[10] = shift(w[10],1); w[12] = shift(w[12],1);
                    drop_booster();
                }
            }
        } else if (family == 2) {
            ++weapon.progress;
            if (weapon.progress % 8 == 0 && weapon.progress / 8 < 9) {
                const int sequence = weapon.progress / 8;
                auto& missile = spawn(player, 0x3f, slot == 2 ? 60 : 80, slot == 2 ? 800 : 857, Behavior::missile_pair);
                weapon.mounts[2] = missile.slot;
                if (slot == 12) {
                    constexpr int search_offsets[]{0,-10,10,-25,25,-40,40,-55,55};
                    missile.words[13] += search_offsets[sequence];
                    search_weapon_target(missile);
                    missile.words[13] -= search_offsets[sequence];
                }
                constexpr std::array<std::array<int, 3>, 9> offsets{{
                    {{500,-500,0}},{{500,-500,0}},{{-500,-500,0}},{{500,500,0}},{{-500,500,0}},
                    {{500,-500,0}},{{-500,-500,0}},{{500,500,0}},{{-500,500,0}}
                }};
                const auto& offset = offsets[sequence];
                place(mounted(sequence % 2 == 0 ? 0 : 1), missile, offset[0], offset[1], offset[2]);
                ++state_.shots;
                sound(missile, 2000);
            }
            if (weapon.progress < 75) { position(0, launcher.extension, MountPose::ready); }
            if (weapon.progress < 80) { position(1, launcher.extension, MountPose::ready); }
            if (weapon.progress == 70) { discard(0); }
            if (weapon.progress == 85) { discard(1); }
            if (weapon.progress == 90) { weapon.phase = deploy; }
        } else if (slot == 11) {
            ++weapon.progress;
            for (int i = 1; i < 4; ++i) {
                if (weapon.progress < i * 8) { position(i, launcher.extension, MountPose::ready); }
                if (weapon.progress == i * 8) { launch(i, Behavior::missile_pair); }
            }
            if (weapon.progress == 25) { weapon.phase = reload; }
        } else if (slot == 10) {
            if (--weapon.progress == 0) {
                weapon.phase = second_release;
            }
            position(0, launcher.extension, MountPose::ready); position(1, launcher.extension, MountPose::ready);
        } else {
            if (--weapon.progress == 0) { weapon.phase = second_release; }
            position(1, launcher.extension, MountPose::ready);
        }
        break;
    case second_release:
        if (slot == 10) {
            launch(0, Behavior::missile_single); launch(1, Behavior::missile_single);
            weapon.phase = reload; weapon.progress = launcher.extension; break;
        }
        launch(1, family == 3 ? Behavior::cruise_missile : Behavior::missile_pair);
        if (family == 3) {
            auto& w = mounted(1).words;
            mounted(ground && slot == 13 ? 0 : 1).words[6] = 0;
            w[5] = 300; w[20] = slot == 3 ? 11 : 21;
        }
        weapon.phase = reload; weapon.progress = launcher.extension;
        break;
    case holster:
        if (ground || hover) { sound(player,3001); }
        if (family == 2) { weapon.progress = launcher.extension; }
        weapon.phase = retracting;
        break;
    case retracting:
        --weapon.progress;
        for (int i = 0; i < launcher.count; ++i) { position(i, weapon.progress, MountPose::moving); }
        if (family == 4) { --mounted(0).words[5]; }
        if (weapon.progress != 0) { break; }
        for (int i = 0; i < launcher.count; ++i) { mounted(i).words[1] = 0; weapon.mounts[launcher.first + i] = no_entity; }
        weapon.phase = change;
        break;
    case change: break;
    default: throw std::runtime_error("Invalid special weapon transition");
    }
}

}
