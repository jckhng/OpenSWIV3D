#include "ground_effects.hpp"
#include "combat_math.hpp"

#include <stdexcept>

namespace swiv3d::detail {
namespace {
using namespace combat_math;

int wrapped(std::int64_t value) {
    const auto bits = static_cast<std::uint32_t>(value);
    return bits <= INT32_MAX ? static_cast<int>(bits) : -1 - static_cast<int>(UINT32_MAX - bits);
}
}

void queue_ground_effect(GroundEffects& effects, const std::array<std::int32_t, 3>& position,
                         GroundEffectKind kind, std::int32_t lifetime) {
    if (effects.free == no_entity) { throw std::runtime_error("Ground effect pool exhausted"); }
    const int slot = effects.free;
    auto& effect = effects.records.at(slot);
    effects.free = effect.next;
    effect.next = effects.active;
    effects.active = slot;
    effect.position[0] = position[0];
    if (kind != GroundEffectKind::teleport) { effect.position[1] = position[1]; }
    effect.position[2] = position[2];
    effect.lifetime = lifetime;
    effect.kind = kind;
}

void advance_ground_effects(GroundEffects& effects) {
    int slot = effects.active;
    effects.active = no_entity;
    while (slot != no_entity) {
        auto& effect = effects.records.at(slot);
        const int next = effect.next;
        if (--effect.lifetime > 0) {
            effect.next = effects.active;
            effects.active = slot;
        } else {
            effect.next = effects.free;
            effects.free = slot;
        }
        slot = next;
    }
}

void apply_ground_effects(const GroundEffects& effects, int x, int z, int& height, int& shade, int& texture) {
    constexpr int angle_count = 512;
    for (int slot = effects.active; slot != no_entity; slot = effects.records.at(slot).next) {
        const auto& effect = effects.records.at(slot);
        const int dx = wrapped(std::int64_t{x} - shift(effect.position[0],8));
        if (effect.kind == GroundEffectKind::pulse_wall) {
            const int phase = wrapped((std::int64_t{effect.lifetime} - 30) * 16 + shift(dx,2) + 128);
            if (phase <= 0 || phase >= 255) { continue; }
            const int top = wrapped(sine(phase) * 2 - 300 + shift(effect.position[1],8));
            if (height < top) { height = top; shade = 1000; texture = 0; }
            continue;
        }
        if (effect.kind != GroundEffectKind::pulse_ring && effect.kind != GroundEffectKind::teleport) { continue; }
        const int dz = wrapped(std::int64_t{z} - shift(effect.position[2],8));
        const auto square = static_cast<std::uint32_t>(dx) * static_cast<std::uint32_t>(dx) +
                            static_cast<std::uint32_t>(dz) * static_cast<std::uint32_t>(dz);
        const bool ring = effect.kind == GroundEffectKind::pulse_ring;
        const int phase = wrapped(shift(wrapped(square),ring ? 9 : 11) +
                                  (std::int64_t{effect.lifetime} - (ring ? 20 : 25)) * 64);
        if (phase > 0 && phase < angle_count) {
            const int wave = shift(wrapped(std::int64_t{sine(phase)} * effect.lifetime),5);
            if (ring) { height = wrapped(std::int64_t{height} - wave); }
            else { shade = wrapped(std::int64_t{shade} + wave); }
        }
        if (ring && shift(phase,1) > 0 && shift(phase,1) < angle_count) {
            shade = wrapped(std::int64_t{shade} + sine(shift(phase,1)));
        }
    }
}

}
