#pragma once

#include <algorithm>
#include <cstdint>

namespace swiv3d::detail {

// The qualified SB16 driver requests 5,000 us. Miles programs mode 2 with
// floor(period * 10000 / 8380). The pinned backend's mode-2 counter reloads at
// terminal count one. Five IRQ0 entries dispatch each game callback.
class CompatibilityClock {
public:
    static constexpr std::uint32_t pit_frequency = 1193182;
    static constexpr std::uint64_t cpu_frequency = 60000000;
    static constexpr std::uint32_t disk_bytes_per_second = 8333333;
    static constexpr std::uint32_t driver_period_us = 5000;
    static constexpr std::uint32_t pit_divisor = driver_period_us * 10000 / 8380;
    static constexpr std::uint32_t pit_period_ticks = pit_divisor - 1;
    static constexpr std::uint32_t interrupts_per_update = 5;
    static constexpr std::uint32_t message_capacity = 160;
    static constexpr std::uint32_t audio_rate = 44100;

    explicit CompatibilityClock(std::uint32_t sample_rate = audio_rate) : sample_rate_(sample_rate) {}

    void elapse(std::uint64_t nanoseconds) {
        constexpr std::uint64_t nanoseconds_per_second = 1000000000;
        const auto audio_partial = (nanoseconds % nanoseconds_per_second) * sample_rate_ + audio_fraction_;
        audio_pending_ += nanoseconds / nanoseconds_per_second * sample_rate_ + audio_partial / nanoseconds_per_second;
        audio_fraction_ = audio_partial % nanoseconds_per_second;
        const auto partial = (nanoseconds % nanoseconds_per_second) * pit_frequency + fraction_;
        const auto ticks = nanoseconds / nanoseconds_per_second * pit_frequency + partial / nanoseconds_per_second;
        fraction_ = partial % nanoseconds_per_second;
        const auto total = ticks + counter_;
        const auto interrupts = total / pit_period_ticks + interrupt_phase_;
        counter_ = total % pit_period_ticks;
        interrupt_phase_ = interrupts % interrupts_per_update;
        pending_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(message_capacity, pending_ + interrupts / interrupts_per_update));
    }

    std::uint32_t drain() {
        const auto result = pending_;
        pending_ = 0;
        return result;
    }

    std::uint64_t drain_audio() {
        const auto result = audio_pending_;
        audio_pending_ = 0;
        return result;
    }

    void dispatched() {
        if (++updates_ < 120) { return; }
        frame_rate_ = std::max(5U, std::max(frames_, 1U) / 3);
        updates_ = frames_ = 0;
    }

    void rendered() { ++frames_; }
    std::uint32_t frame_rate() const { return frame_rate_; }

private:
    std::uint32_t sample_rate_;
    std::uint64_t fraction_ = 0, counter_ = 0, interrupt_phase_ = 0;
    std::uint64_t audio_fraction_ = 0, audio_pending_ = 0;
    std::uint32_t pending_ = 0, updates_ = 0, frames_ = 0, frame_rate_ = 40;
};

}
