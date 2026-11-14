#pragma once

#include "swiv3d/assets.hpp"

#include <algorithm>
#include <stdexcept>

namespace scenario {
inline swiv3d::Archive enemy_gun(const swiv3d::Archive& original, int gun) {
    if (gun < 0 || gun > 701) { throw std::runtime_error("Invalid enemy gun fixture"); }
    std::vector<std::uint8_t> bytes;
    for (const auto& entry : original.resources()) {
        bytes.resize(std::max(bytes.size(), std::size_t(entry.offset) + entry.size));
        const auto member = original.load(entry.id);
        std::copy(member.begin(), member.end(), bytes.begin() + entry.offset);
    }
    const auto put = [&](std::size_t offset, unsigned value) {
        for (unsigned i = 0; i < 4; ++i) { bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8)); }
    };
    for (const auto& entry : original.resources()) {
        if (entry.id != 105) { continue; }
        for (std::size_t offset = entry.offset; offset + 64 <= entry.offset + entry.size; offset += 64) {
            const auto command = unsigned(bytes[offset]) | unsigned(bytes[offset + 1]) << 8;
            if (command == 3000 || command == 5000 || command == 5002) { put(offset + 28, gun); }
        }
    }
    for (const auto& entry : original.resources()) {
        const auto offset = bytes.size(); bytes.resize(offset + 12);
        put(offset, entry.offset); put(offset + 4, entry.size); put(offset + 8, entry.id);
    }
    const auto footer = bytes.size(); bytes.resize(footer + 12);
    put(footer, static_cast<unsigned>(original.resources().size() + 1));
    return swiv3d::Archive(std::move(bytes));
}
}
