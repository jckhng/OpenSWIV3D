#include "flight.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace swiv3d::detail {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr std::uint32_t angle_mask = 511;
constexpr std::uint32_t world_mask = 0x7fffff;
constexpr int helicopter_clearance = 80;
constexpr int pitch_input = 1200;
constexpr int strafe_input = 800;
constexpr int turn_input = 600;
constexpr int turn_bank = 200;

enum Binding : std::size_t { left, right, forward, backward, strafe_left, strafe_right, fire,
                            special, previous_weapon, next_weapon, slide };

std::int32_t signed_word(std::uint32_t value) {
    return static_cast<std::int32_t>(value <= INT32_MAX ? static_cast<std::int64_t>(value) :
                                                        static_cast<std::int64_t>(value) - (INT64_C(1) << 32));
}

int add(int a, int b) { return signed_word(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b)); }
int multiply(int a, int b) { return signed_word(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b)); }
int shift(int value, unsigned bits) {
    const auto divisor = INT64_C(1) << bits;
    return static_cast<int>(value >= 0 ? value / divisor : -((-static_cast<std::int64_t>(value) + divisor - 1) / divisor));
}
int sine(int angle) { return static_cast<int>(std::round(256 * std::sin((static_cast<std::uint32_t>(angle) & angle_mask) * 2 * pi / 512))); }
int cosine(int angle) { return sine(add(angle, 128)); }

void body(std::array<std::int32_t, 27>& w, FlightState& flight,
          const std::function<std::int32_t(std::int32_t, std::int32_t)>& height, int clearance = helicopter_clearance) {
    for (auto field : {7U, 8U, 9U}) {
        w[field] = add(w[field], w[field + 3]);
    }
    if (w[15] < 0) {
        w[15] = add(w[15], 4);
    }
    if (w[15] > 0) {
        w[15] = add(w[15], -4);
    }
    if (w[5] > 0) {
        --w[5];
    }
    if (w[14] < 0) {
        w[14] = add(w[14], 2);
    }
    if (w[14] > 0) {
        w[14] = add(w[14], -2);
    }
    w[10] = add(w[10], -shift(multiply(sine(w[13]), w[14]), 5));
    w[12] = add(w[12], -shift(multiply(cosine(w[13]), w[14]), 5));
    w[10] = add(w[10], shift(multiply(sine(add(w[13], 128)), w[15]), 6));
    w[12] = add(w[12], shift(multiply(cosine(add(w[13], 128)), w[15]), 6));
    const int ground = height(w[7], w[9]);
    const int ahead = height(add(w[7], multiply(w[10], 32)), add(w[9], multiply(w[12], 32)));
    const int error = add(add(shift(std::max(ground, ahead), 8), clearance), -shift(w[8], 8));
    if (error > 0) {
        w[11] = add(w[11], multiply(error, 2));
    }
    if (error < 0) {
        w[11] = add(w[11], shift(error, 1));
    }
    for (auto field : {10U, 11U, 12U}) {
        const int drag = shift(w[field], 4);
        w[field] = add(w[field], -(drag == 0 ? w[field] : drag));
    }
    if (add(w[8], -2000) <= ground) {
        int direction = 0;
        int lowest = INT32_MAX;
        constexpr int directions = 16;
        for (int i = 0; i < directions; ++i) {
            const auto sample = height(add(w[7], sine(i * 32)), add(w[9], cosine(i * 32)));
            if (sample < lowest) {
                lowest = sample;
                direction = i * 32;
            }
        }
        w[10] = multiply(sine(direction), 16);
        w[11] = 1000;
        w[12] = multiply(cosine(direction), 16);
        w[2] = std::max(add(w[2], -10), 0);
        ++flight.ground_impacts;
    }
    w[7] = static_cast<int>(static_cast<std::uint32_t>(w[7]) & world_mask);
    w[9] = static_cast<int>(static_cast<std::uint32_t>(w[9]) & world_mask);
    w[6] = add(w[6], 1);
}

void control(EntityState& player, FlightState& flight, const KeyboardState& keys, const EntityState& target) {
    auto& w = player.words;
    const auto held = [&](Binding binding) { return keys.at(flight.bindings[binding]); };
    int pitch = 0;
    int bank = 0;
    int turn = 0;
    ++flight.control_ticks;
    const auto analog = [&](std::size_t role) { return flight.controller != ControllerKind::keyboard && flight.axis_bindings[role] != 0; };
    const auto axis = [&](std::size_t role) { return flight.axes.at(flight.axis_bindings[role] - 1); };
    if (analog(1)) { pitch = axis(1) * 10; }
    else {
        if (held(forward)) { pitch += pitch_input; }
        if (held(backward)) { pitch -= pitch_input; }
    }
    if (held(slide)) {
        if (analog(0)) { bank = axis(0) * 7; }
        else {
            if (held(left)) { bank -= strafe_input; }
            if (held(right)) { bank += strafe_input; }
        }
    } else {
        if (analog(2)) { bank = axis(2) * 7; }
        else {
            if (held(strafe_left)) { bank -= strafe_input; }
            if (held(strafe_right)) { bank += strafe_input; }
        }
        if (analog(0)) {
            turn += axis(0) * 8;
            bank += shift(axis(0) * 800, 8);
        } else {
            if (held(left)) { bank -= turn_bank; turn -= turn_input; }
            if (held(right)) { bank += turn_bank; turn += turn_input; }
        }
    }
    flight.previous_heading = w[13];
    if (target.slot != player.slot && w[8] < target.words[8]) {
        w[8] = add(w[8], 100);
    }
    flight.attitude[0] = add(flight.attitude[0], turn);
    const int next_pitch = add(flight.attitude[1], -pitch);
    const int next_bank = add(flight.attitude[2], bank);
    w[13] = shift(flight.attitude[0], 8);
    w[14] = shift(next_pitch, 8);
    w[15] = shift(next_bank, 7);
    flight.attitude[1] = shift(multiply(next_pitch, 14), 4);
    flight.attitude[2] = shift(multiply(next_bank, 14), 4);
    w[8] = std::min(w[8], flight.maximum_height);
    flight.engine_pitch = add(add(shift(w[11], 1), 10000), shift(add(multiply(w[10], w[10]), multiply(w[12], w[12])), 14));
    if (flight.craft != CraftKind::hover && add(target.words[8], -w[8]) > 15000) {
        w[11] = add(shift(multiply(w[11], 14), 4), 40);
    }
}

}

void advance_helicopter(EntityState& player, FlightState& flight, const KeyboardState& keys,
                        const std::function<std::int32_t(std::int32_t, std::int32_t)>& height) {
    body(player.words, flight, height);
    control(player, flight, keys, player);
}

void move_helicopter(EntityState& player, FlightState& flight,
                     const std::function<std::int32_t(std::int32_t, std::int32_t)>& height) {
    body(player.words, flight, height);
}

void move_aircraft(EntityState& actor, int clearance,
                   const std::function<std::int32_t(std::int32_t, std::int32_t)>& height) {
    FlightState scratch;
    body(actor.words, scratch, height, clearance);
}

void control_helicopter(EntityState& player, FlightState& flight, const KeyboardState& keys, const EntityState& target) {
    control(player, flight, keys, target);
}

void search_flight_target(MissionState& state) {
    const auto entity = [&](int slot) -> EntityState& {
        const auto found = std::find_if(state.entities.begin(), state.entities.end(), [&](const EntityState& candidate) { return candidate.slot == slot; });
        if (found == state.entities.end()) {
            throw std::runtime_error("Flight target/grid link references an unallocated entity");
        }
        return *found;
    };
    auto& player = entity(state.player);
    auto& w = player.words;
    const int vx = shift(w[10], 8), vz = shift(w[12], 8);
    const int speed = add(multiply(vx, vx), multiply(vz, vz));
    state.object_direction = {speed, vz};
    const int interval = speed < 30 ? 60 : speed < 140 ? 35 : 7;
    if (w[6] <= interval) {
        return;
    }
    std::vector<int> candidates;
    constexpr int radius = 3;
    const int cell_x = shift(w[7], 17), cell_z = shift(w[9], 17);
    for (int z = cell_z - radius; z <= cell_z + radius; ++z) {
        for (int x = cell_x - radius; x <= cell_x + radius; ++x) {
            for (auto slot = state.grid[(z & 63) * 64 + (x & 63)]; slot != no_entity; slot = entity(slot).words[0]) {
                if ((entity(slot).words[1] & 0x44) == 0) {
                    candidates.push_back(slot);
                }
            }
        }
    }
    w[22] = player.slot;
    int best = 50000;
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        const auto& other = entity(*it).words;
        int dx = shift(add(other[7], -w[7]), 8), dz = shift(add(other[9], -w[9]), 8);
        auto square = static_cast<std::uint32_t>(add(multiply(dx, dx), multiply(dz, dz))) >> 11;
        if (square == 0 || square > 4000) {
            square = 4000;
        }
        const int root = static_cast<int>(std::sqrt(square * 2048.0));
        const int reciprocal = 65535 / root;
        dx = shift(multiply(dx, reciprocal), 8);
        dz = shift(multiply(dz, reciprocal), 8);
        const int bias = (other[1] & 0x1000) != 0 ? 4000 : (other[1] & 8) != 0 ? -6000 : (other[1] & 0x4000) != 0 ? -4000 : 0;
        const int score = add(add(multiply(sine(w[13]), dx), multiply(cosine(w[13]), dz)), bias);
        if (score > best) {
            best = score;
            w[22] = *it;
        }
    }
    w[6] = 0;
}

}
