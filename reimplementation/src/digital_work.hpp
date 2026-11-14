#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace swiv3d::detail::digital_work {

constexpr unsigned idle_service = 29, retained_service = 37;
constexpr unsigned driver_stop = 660, stop_caller = 10;
constexpr unsigned mixer_rate = 11025;

struct Merge { unsigned setup, frame; };

// AILSSA_merge and its stereo-output kernels, with signed 16-bit input.
inline Merge merge(unsigned bits, unsigned channels, int rate, int volume, int pan, int master) {
    constexpr std::uint64_t unity = 65536;
    const auto step = std::uint64_t(std::max(1,rate)) * unity / mixer_rate;
    const auto difference = step > unity ? step - unity : unity - step;
    const bool resampled = difference > 655;
    const bool scaled = volume != 127 || master != 127 || pan != 64;
    const bool stereo = channels == 2;
    const bool wide = bits == 16;
    unsigned work = 13 + (wide ? (stereo ? 12 : 7) : (stereo ? 9 : 8));
    work += 2 + (stereo ? 2 : 0) + 2;
    if (volume == 127) { work += 2; }
    if (volume == 127 && master == 127) { work += 2; }
    if (scaled) { work += 3 + (wide ? 3 : 0); }
    else { work += 2 + (wide ? 1 : 0); }
    work += 5 + (rate <= 0 ? 1 : 0) + 6 + (step <= unity ? 1 : 0) + 2;
    if (resampled) { work += 13 + (stereo ? 2 : 0) + (wide ? 2 : 0); }
    work += 34;
    constexpr unsigned kernels[2][2][2][2]{
        {{{11,12},{11,12}},{{14,17},{12,15}}},
        {{{10,13},{15,18}},{{12,15},{16,19}}}
    };
    return {work,kernels[wide][stereo][scaled][resampled]};
}

// SS_SERVE, 16 sample slots, AILSSA_flush_buffer and unsigned stereo DMA copy.
// REP dispatches use the normal core's four-unit limit. Event-budget splitting
// and the DOS/4GW driver-stop call are separate scheduling costs.
inline std::uint64_t refill(unsigned active, const std::vector<std::int32_t>& accumulation) {
    constexpr unsigned callback = 244, flush = 2586, dma_dispatch = 37;
    auto work = std::uint64_t(callback + active * 2 + (active == 0 ? 3 : 0) + flush + dma_dispatch);
    const auto frames = accumulation.size() / 2;
    work += frames * (active > 1 ? 18 : 10) + 1 + (active > 1 ? 1 : 0);
    if (active > 1) {
        for (auto value : accumulation) {
            if (value < -32768) { work += 2; }
        }
    }
    return work;
}

}
