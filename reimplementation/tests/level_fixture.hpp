#pragma once
#include "swiv3d/assets.hpp"
#include <algorithm>
#include <stdexcept>

namespace scenario {
enum class Arrival { station, burst_factory, intruder_factory, ski_station, boss, boss2, pyramid, eye, mars_face, ground_exit, teleport };
inline swiv3d::Archive arrival_start(const swiv3d::Archive& original, Arrival arrival, unsigned arrival_index = 0) {
    const unsigned setup = arrival == Arrival::boss2 ? 8010 : arrival == Arrival::pyramid ? 8020 : arrival == Arrival::eye ? 3200 :
                           arrival == Arrival::mars_face ? 8030 : arrival == Arrival::ground_exit ? 7020 : arrival == Arrival::teleport ? 7801 :
                           arrival == Arrival::boss ? 8000 : arrival == Arrival::station ? 7010 : arrival == Arrival::ski_station ? 7013 :
                           arrival == Arrival::burst_factory ? 6101 : 6300;
    std::vector<std::uint8_t> bytes;
    for (const auto& entry : original.resources()) {
        bytes.resize(std::max(bytes.size(), std::size_t(entry.offset) + entry.size));
        const auto member = original.load(entry.id);
        std::copy(member.begin(), member.end(), bytes.begin() + entry.offset);
    }
    const auto get = [&](std::size_t offset) {
        return unsigned(bytes.at(offset)) | unsigned(bytes.at(offset + 1)) << 8 |
               unsigned(bytes.at(offset + 2)) << 16 | unsigned(bytes.at(offset + 3)) << 24;
    };
    const auto put = [&](std::size_t offset, unsigned value) {
        for (unsigned i = 0; i < 4; ++i) { bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8)); }
    };
    const auto command = std::find_if(original.resources().begin(), original.resources().end(), [](const auto& entry) { return entry.id == 105; });
    if (command == original.resources().end()) { throw std::runtime_error("No mission commands for station fixture"); }
    std::size_t player = 0, station = 0;
    unsigned candidate = 0;
    for (std::size_t offset = command->offset; offset + 64 <= command->offset + command->size; offset += 64) {
        if (get(offset) == 1000 && player == 0) { player = offset; }
        const bool matching = get(offset) == setup || (arrival == Arrival::boss && get(offset) == 8001);
        if (matching && candidate++ == arrival_index) { station = offset; }
    }
    if (player == 0 || station == 0) { throw std::runtime_error("No requested arrival fixture"); }
    put(player + 4, get(station + 4) + (arrival == Arrival::intruder_factory ? 256 : arrival == Arrival::teleport ? 32 : 64));
    put(player + 8, get(station + 8));
    for (const auto& entry : original.resources()) {
        const auto offset = bytes.size(); bytes.resize(offset + 12);
        put(offset, entry.offset); put(offset + 4, entry.size); put(offset + 8, entry.id);
    }
    const auto footer = bytes.size(); bytes.resize(footer + 12);
    put(footer, static_cast<unsigned>(original.resources().size() + 1));
    return swiv3d::Archive(std::move(bytes));
}
inline swiv3d::Archive station_start(const swiv3d::Archive& original) { return arrival_start(original,Arrival::station); }
}
