#include "combat.hpp"
#include "combat_math.hpp"
#include "combat_jitter.hpp"
#include "ground_effects.hpp"

namespace swiv3d::detail {
namespace {
using namespace combat_math;
}

void Combat::mars_actor(EntityState& actor) {
    auto& w = actor.words;
    const auto behavior = static_cast<Behavior>(w[26]);
    if (behavior == Behavior::teleport) {
        if (w[20] == 0) { w[13] -= 16; return; }
        if (w[20] != 10) { return; }
        w[13] += 16;
        if ((w[6] & 31) == 0) { scan_player(actor,6000000); }
        if (w[22] == actor.slot) { return; }
        auto& player = entity(w[22]);
        const int dx = shift(w[7] - player.words[7],8), dz = shift(w[9] - player.words[9],8);
        if (dx * dx + dz * dz >= 4000) { return; }
        const auto destination = std::find_if(state_.entities.rbegin(),state_.entities.rend(),[&](const auto& candidate) {
            return static_cast<Behavior>(candidate.words[26]) == Behavior::teleport && candidate.words[20] == 0 && candidate.words[21] == w[21];
        });
        if (destination == state_.entities.rend()) { return; }
        std::copy(destination->words.begin() + 7,destination->words.begin() + 10,player.words.begin() + 7);
        player.words[8] += 40000;
        queue_ground_effect(state_.ground_effects,{{player.words[7],player.words[8],player.words[9]}},GroundEffectKind::teleport,40);
        constexpr int fragments = 20;
        for (int i = 0; i < fragments; ++i) {
            auto& fragment = spawn(player,0x40,20,10001,Behavior::debris_large);
            const auto& vector = combat_jitter[state_.effects_phase];
            fragment.words[10] = vector[0] * 30; fragment.words[11] = std::abs(vector[1]) * 30; fragment.words[12] = vector[2] * 30;
            state_.effects_phase = (state_.effects_phase + 1) & 255;
            fragment.words[13] = combat_jitter[state_.effects_phase][0];
        }
        player.words[11] = 0; state_.camera_jump = true;
        active_regions_.push_front({actor.slot,2});
        return;
    }
    if (behavior == Behavior::stationary_tank) {
        search(actor,40,2);
        if (w[2] < 1) {
            w[1] = 0; ++entity(w[23]).words[20]; award(actor,59);
            effect(actor,Behavior::explosion,27); check_objective(actor);
        }
        return;
    }
    if (behavior == Behavior::stationary_top) {
        auto& parent = entity(w[23]).words;
        std::copy(parent.begin() + 7,parent.begin() + 10,w.begin() + 7);
        if (w[5] > 0) { --w[5]; }
        w[22] = parent[22];
        if (w[22] == w[23]) {
            if (parent[13] < w[13]) { --w[13]; }
            if (w[13] < parent[13]) { ++w[13]; }
        } else { aim(actor); }
        if (w[2] < 1) { w[1] = 0; }
        if (w[23] != parent[23]) { w[14] = std::clamp(w[14],-30,128); }
        if (w[23] == actor.slot) {
            w[11] -= 30; w[13] += 25;
            if (w[8] <= height_(w[7],w[9])) { w[1] = 0; }
        } else if (parent[2] < 1) { w[11] = 1200; w[23] = actor.slot; w[1] |= 0x4012; }
        entity(w[23]).words[13] = w[13];
        return;
    }
    if (behavior == Behavior::trailer) {
        settle(actor,ground_contact(actor,0));
        if (entity(w[23]).words[1] == 0) { w[23] = actor.slot; }
        search(actor,40);
        if (w[22] != actor.slot && w[5] > 0) { --w[5]; }
        if (w[23] != actor.slot) {
            const auto& parent = entity(w[23]).words;
            const int dx = parent[7] - sine(parent[13]) * 16 - (w[7] + sine(w[13]) * 32);
            const int dz = parent[9] - cosine(parent[13]) * 16 - (w[9] + cosine(w[13]) * 32);
            const auto product = static_cast<std::int32_t>(static_cast<std::uint32_t>(
                std::int64_t(sine(w[13] + 128)) * dx + std::int64_t(cosine(w[13] + 128)) * dz));
            const int heading = w[13] + shift(product,15);
            w[13] = std::clamp(heading,parent[13] - 60,parent[13] + 60);
            w[7] = parent[7] - sine(parent[13]) * 16 - sine(w[13]) * 32;
            w[9] = parent[9] - cosine(parent[13]) * 16 - cosine(w[13]) * 32;
        }
        if (w[2] < 1) { w[1] = 0; award(actor,59); effect(actor,Behavior::explosion,27); }
        return;
    }
    if (w[8] <= height_(w[7],w[9]) || w[2] < 1) { w[1] = 0; effect(actor,Behavior::explosion,27); }
    const int phase = w[20];
    auto& parent = entity(w[23]);
    if (phase == 0 || phase == 1 || phase == 10 || phase == 20) {
        w[13] = parent.words[13]; w[15] = parent.words[15];
        if (phase == 0 || phase == 1) {
            place(parent,actor,phase == 0 ? 4000 : -4000,3000,-5000);
            w[22] = parent.words[22]; w[14] = parent.words[14] + 40; w[5] = 40;
            if (parent.words[5] < (phase == 0 ? 201 : 71) && w[22] != w[23]) { w[20] = phase == 0 ? 10 : 20; }
        } else {
            w[14] = parent.words[14] + ++w[5];
            if (w[5] > 127) { ++w[20]; }
            place(parent,actor,phase == 10 ? 4000 : -4000,3000,-5000);
        }
        if ((phase != 20) && parent.words[1] == 0) { w[1] = 0; }
        return;
    }
    if (phase == 11 || phase == 21) {
        w[13] = parent.words[13]; w[15] = parent.words[15]; sound(actor,2000);
        w[1] = 0x3b; w[20] = 30; w[10] = w[12] = 0; w[11] = 600; w[14] = 128; w[5] = 30;
        return;
    }
    if (phase < 30 || phase > 33) { return; }
    --w[5]; trail(actor,3);
    if (phase == 30) {
        const auto& target = entity(w[22]).words;
        steer_towards(actor,target[7],target[9],5);
    } else {
        velocity(actor,phase == 33 ? 5000 : 900);
        if (entity(w[22]).words[1] == 0) { w[22] = w[23]; }
        if (w[22] != w[23] && w[22] != actor.slot && w[6] < 6) {
            face(actor,phase == 31 ? 14 : phase == 32 ? 2 : 4); w[15] = 0;
        }
    }
    if (w[5] < 1) {
        if (phase == 30 || phase == 31) { w[5] = 13; }
        if (phase == 32) { w[5] = 150; }
        ++w[20];
    }
}

}
