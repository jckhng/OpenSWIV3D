#pragma once

#include "swiv3d/mission.hpp"

#include <algorithm>
#include <cmath>

namespace swiv3d::detail::combat_math {

constexpr double pi = 3.14159265358979323846;

inline int shift(std::int64_t value, unsigned bits) {
    const auto divisor = INT64_C(1) << bits;
    return static_cast<int>(value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor));
}

inline int sine(int angle) { return static_cast<int>(std::round(256 * std::sin((angle & 511) * 2 * pi / 512))); }

inline int cosine(int angle) { return sine(angle + 128); }

inline int word(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    const auto value = static_cast<std::uint32_t>(bytes.at(offset)) | static_cast<std::uint32_t>(bytes.at(offset + 1)) << 8 |
        static_cast<std::uint32_t>(bytes.at(offset + 2)) << 16 | static_cast<std::uint32_t>(bytes.at(offset + 3)) << 24;
    return value <= INT32_MAX ? static_cast<int>(value) : -1 - static_cast<int>(UINT32_MAX - value);
}

inline std::array<int, 3> rotate(const EntityState& actor, const std::array<int, 3>& local) {
    const auto& w = actor.words;
    const int sp = sine(-w[14]), cp = cosine(-w[14]), sy = sine(-w[13]), cy = cosine(-w[13]);
    const int y = shift(cp * local[1] - sp * local[2], 8);
    const int z = shift(cp * local[2] + sp * local[1], 8);
    return {{shift(cy * local[0] - sy * z, 8), y, shift(sy * local[0] + cy * z, 8)}};
}

inline void velocity(EntityState& actor, int speed) {
    const auto v = rotate(actor, {{0,0,speed}});
    std::copy(v.begin(), v.end(), actor.words.begin() + 10);
}

inline std::array<int, 3> normalize(std::array<int, 3> vector) {
    std::uint32_t square = 0;
    for (int d : vector) { square += static_cast<std::uint32_t>(d) * static_cast<std::uint32_t>(d); }
    square >>= 11;
    if (square == 0 || square > 4000) { square = 4000; }
    const int root = static_cast<int>(std::sqrt(square * 2048.0));
    const int reciprocal = 65535 / root;
    for (auto& d : vector) { d = shift(static_cast<std::int32_t>(static_cast<std::uint32_t>(d) * reciprocal), 8); }
    return vector;
}

}
