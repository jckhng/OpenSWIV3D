#pragma once

#include "clock.hpp"
#include "device_audio.hpp"

namespace swiv3d::detail {

class SoundBlasterDma {
public:
    static constexpr std::size_t block_frames = 1024;
    enum class Service { none, idle, retained, refill, stopped };
    using Block = std::array<StereoSample,block_frames>;

    void start() {
        driver_playing_ = true;
        stop_pending_ = false;
        playing_ = true;
    }

    // Miles services the inactive half on IRQ0. The SB IRQ only advances the
    // active half. Source fetches retain the device resampler's lookahead.
    Service elapse_frame(const std::function<bool(Block&)>& produce) {
        Service result = Service::none;
        elapse_cycles(CompatibilityClock::cpu_frequency / device_audio_rate,produce,
            [&](auto service) { result = service; });
        return result;
    }

    void elapse_cycles(std::uint64_t cycles, const std::function<bool(Block&)>& produce,
                       const std::function<void(Service)>& serviced) {
        constexpr auto period = std::uint64_t(CompatibilityClock::pit_period_ticks) * CompatibilityClock::cpu_frequency;
        phase_ += cycles * CompatibilityClock::pit_frequency;
        while (phase_ >= period) {
            phase_ -= period;
            serviced(service(produce));
        }
    }

private:
    Service service(const std::function<bool(Block&)>& produce) {
        ++timer_interrupts_;
        const auto current = position_ / block_frames;
        if (!driver_playing_) { return Service::idle; }
        if (current == served_half_) { return Service::retained; }
        served_half_ = current;
        if (produce(blocks_[current ^ 1])) { empty_blocks_ = 0; return Service::refill; }
        if (++empty_blocks_ == empty_block_limit) {
            driver_playing_ = false;
            stop_pending_ = true;
            return Service::stopped;
        }
        return Service::refill;
    }

public:
    StereoSample next() {
        if (!playing_) { return {}; }
        const auto sample = blocks_[position_ / block_frames][position_ % block_frames];
        if (++position_ % block_frames == 0) {
            ++interrupts_;
            if (stop_pending_) { playing_ = false; }
        }
        position_ %= block_frames * half_count;
        return sample;
    }

    std::uint64_t timer_milliseconds(std::uint64_t cycles_before = 0) const {
        // AIL_ms_count combines requested timer periods with the latched PIT
        // interpolation. Its clock runs slightly faster than elapsed wall time.
        constexpr unsigned miles_pit_frequency = 0x1234dd;
        constexpr auto period = std::uint64_t(CompatibilityClock::pit_period_ticks) * CompatibilityClock::cpu_frequency;
        const auto back = cycles_before * CompatibilityClock::pit_frequency;
        auto phase = phase_;
        auto interrupts = timer_interrupts_;
        if (back > phase) {
            if (interrupts == 0 || back - phase > period) { throw std::runtime_error("Movie timestamp precedes device clock"); }
            --interrupts;
            phase += period;
        }
        const auto ticks = (phase - back) / CompatibilityClock::cpu_frequency;
        return interrupts * CompatibilityClock::driver_period_us / 1000 + ticks * 1000 / miles_pit_frequency;
    }

    std::uint64_t interrupts() const { return interrupts_; }
    bool playing() const { return playing_; }

private:
    static constexpr std::size_t half_count = 2;
    static constexpr unsigned empty_block_limit = 3;
    std::array<Block,half_count> blocks_{};
    std::uint64_t phase_ = 0, interrupts_ = 0, timer_interrupts_ = 0;
    std::size_t position_ = 0, served_half_ = half_count;
    unsigned empty_blocks_ = 0;
    bool playing_ = false, driver_playing_ = false, stop_pending_ = false;
};

}
