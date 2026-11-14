#include "combat.hpp"
#include "combat_math.hpp"

namespace swiv3d::detail {
namespace {
using namespace combat_math;
constexpr int projectile_flags = 0x3b;
}

void Combat::sound(const EntityState& actor, int id) {
    state_.sound_events.push_back({state_.tick, actor.slot, id});
}

void Combat::fire(EntityState& actor) {
    auto& w = actor.words;
    const int gun = w[21], phase = w[5];
    const auto shot = [&](int damage, int model, std::array<int, 3> local, int sound_id,
                          Behavior behavior = Behavior::enemy_bullet, int side = 0) -> EntityState& {
        auto& bullet = spawn(actor, projectile_flags, damage, model, behavior);
        if (side != 0) {
            bullet.words[7] += sine(w[13] + 128) * side;
            bullet.words[9] += cosine(w[13] + 128) * side;
        }
        if (behavior == Behavior::enemy_bullet) {
            const auto v = rotate(bullet, local);
            std::copy(v.begin(), v.end(), bullet.words.begin() + 10);
        }
        if (sound_id != 0) { sound(bullet, sound_id); }
        ++state_.shots;
        return bullet;
    };
    if (gun == 700 || gun == 701) {
        const int charge = gun == 700 ? 30 : 40;
        if (w[5] == 0) { w[5] = charge + 70; }
        if (w[5] == charge) { sound(actor, gun == 700 ? 2004 : 2005); }
        if (w[5] >= charge) { return; }
        w[1] &= 0xeffff;
        if ((w[5] & 7) != 0) { return; }
        auto& target = entity(w[22]).words;
        target[2] -= gun == 700 ? 11 : 19;
        target[1] |= 0x20000;
        const auto normal = normalize({{shift(target[7] - w[7], 6), shift(target[8] - w[8], 6), shift(target[9] - w[9], 6)}});
        for (int axis = 0; axis < 3; ++axis) { target[10 + axis] += normal[axis] * 8; }
        return;
    }
    if (gun >= 600 && gun <= 603) {
        const bool paired = gun == 601 || gun == 603;
        if (phase != 0 && !(paired && phase == 5)) { return; }
        auto& bullet = shot(gun < 602 ? 50 : gun == 602 ? 25 : 35, gun < 602 ? 10001 : 10005, {{0,0,0}}, 0);
        const auto& target = entity(w[22]).words;
        const auto normal = normalize({{shift(target[7] - w[7], 6), shift(target[8] - w[8], 6), shift(target[9] - w[9], 6)}});
        for (int axis = 0; axis < 3; ++axis) { bullet.words[10 + axis] = normal[axis] * 32; }
        if (phase == 0) {
            constexpr int cooldown[]{60,45,55,40};
            w[5] = cooldown[gun - 600];
            sound(bullet, 2002);
        }
        return;
    }
    if (gun >= 610 && gun <= 612) {
        if (phase != 0) { return; }
        auto& bullet = shot(50, 801, {{0,0,0}}, 0, Behavior::polaris_missile);
        bullet.words[10] = bullet.words[12] = 0; bullet.words[11] = 600;
        bullet.words[20] = (gun - 610) * 10; bullet.words[5] = 40; bullet.words[14] = 128;
        w[5] = 60;
        sound(bullet, 2000);
        return;
    }
    if (gun == 405 || gun == 408 || gun == 510 || gun == 511) {
        const bool delayed_pair = gun >= 510, paired_callback = gun == 408 || gun == 511;
        if (phase != 0 && !(delayed_pair && phase == 1)) { return; }
        auto& bullet = shot(55, paired_callback ? 801 : 800, {{0,0,0}}, 0,
                            paired_callback ? Behavior::missile_pair : Behavior::enemy_missile,
                            delayed_pair ? (phase == 0 ? 8 : -8) : 0);
        sound(bullet, 2000);
        if (phase == 0) { w[5] = paired_callback ? 41 : 39; }
        return;
    }
    if (gun == 100 || gun == 400 || gun == 500 || gun == 501) {
        if (phase != 0) { return; }
        const bool double_barrel = gun >= 500;
        const int damage = gun == 400 ? 55 : gun == 501 ? 45 : 35;
        const int model = gun == 400 ? 800 : gun == 501 ? 10001 : 10005;
        const int id = gun == 400 ? 2003 : gun == 501 ? 2002 : 2001;
        auto& first = shot(damage, model, {{0,0,gun == 400 ? 5000 : 9000}}, 0, Behavior::enemy_bullet, double_barrel ? 8 : 0);
        const int sound_actor = double_barrel ? shot(damage, model, {{0,0,9000}}, 0, Behavior::enemy_bullet, -8).slot : first.slot;
        sound(entity(sound_actor), id);
        w[5] = gun == 400 ? 37 : gun == 501 ? 47 : 50;
        return;
    }
    if (gun == 101) {
        if (phase == 1 || phase == 5 || phase == 10) {
            shot(35, 10005, {{200,0,9300}}, 0);
            shot(35, 10005, {{-200,0,9300}}, 2001);
        }
        if (phase == 0) { w[5] = 120; }
        return;
    }
    if (gun == 109 || gun == 110) {
        if (phase == (gun == 109 ? 0 : 1) || phase == (gun == 109 ? 3 : 5)) { shot(45, 10000, {{0,0,9000}}, 2002); }
        if (phase == 0) { w[5] = gun == 109 ? 15 : 35; }
        return;
    }
    if (gun == 111) {
        if (phase == 7) { shot(45, 10000, {{0,150,9500}}, 2002); }
        if (phase == 4) { shot(45, 10000, {{-100,0,9500}}, 2002); }
        if (phase == 1) { shot(45, 10000, {{100,0,9500}}, 2002); }
        if (phase == 0) { w[5] = 33; }
        return;
    }
    if (gun == 112) {
        if (phase == 1 || phase == 3 || phase == 5 || phase == 7) {
            const int spread = (12 - phase) * 64;
            shot(35, 10000, {{spread,0,9300}}, 0); shot(35, 10000, {{-spread,0,9300}}, 0);
            shot(35, 10000, {{0,spread,9300}}, 0); shot(35, 10000, {{0,-spread,9300}}, 2002);
        }
        if (phase == 0) { w[5] = 120; }
        return;
    }
    if (gun == 120 || gun == 121) {
        if (phase >= (gun == 120 ? 20 : 35) || (phase & 1) != 0) { return; }
        const int angle = (phase & 15) * 32;
        shot(60, 10001, {{gun == 120 ? sine(angle) * 2 : sine(angle), gun == 120 ? 0 : cosine(angle), 7000}},
             (phase & 3) == 0 ? 2002 : 0);
        if (phase == 0) { w[5] = 150; }
    }
}

}
