#pragma once

#include "combat_math.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>
#include <utility>
#include <optional>

namespace swiv3d::detail {

constexpr std::uint32_t device_audio_rate = 48000;
using StereoSample = std::array<std::int16_t,2>;
using DeviceSample = std::array<std::int32_t,2>;
enum class DeviceFilter { none, sound_blaster };

// Channel interpolation and filter history precede the final stereo sum.
// Pending values retain the device's already-interpolated lookahead at a
// resumed boundary. Ordinary playback starts with an empty channel.
struct DeviceChannelState {
    std::uint32_t phase = 0;
    std::optional<std::uint32_t> interpolation_phase;
    bool loaded = false;
    StereoSample previous{}, current{};
    DeviceSample history{};
    std::vector<DeviceSample> pending;
};

class DeviceChannel {
public:
    DeviceChannel(std::uint32_t source_rate, DeviceFilter filter, DeviceChannelState state = {})
        : rate_(source_rate), filter_(filter), state_(std::move(state)) {
        if (rate_ == 0 || rate_ > device_audio_rate) { throw std::runtime_error("Unsupported device source rate"); }
        state_.interpolation_phase = state_.interpolation_phase.value_or(state_.phase);
    }

    static DeviceChannel sound_blaster(std::uint32_t source_rate) {
        // Miles tests one unsigned 16-bit DMA unit containing 0x0080.
        // A single source sample primes the DAC without interpolating it.
        constexpr std::int16_t probe_sample = 0x0080 - 0x8000;
        DeviceChannelState state;
        state.loaded = true;
        state.phase = device_audio_rate;
        state.current = {probe_sample,probe_sample};
        DeviceChannel channel(source_rate,DeviceFilter::sound_blaster,state);
        const DeviceSample settled{probe_sample,probe_sample};
        while (channel.state_.history != settled) { channel.silence(); }
        return channel;
    }

    void disable() { state_.phase = 0; }

    std::size_t source_frames_needed(std::size_t frames) const {
        const auto pending = state_.pending.size() - pending_;
        if (pending >= frames) { return 0; }
        return ((frames - pending) * rate_ + state_.phase + device_audio_rate - 1) / device_audio_rate + !state_.loaded;
    }

    void prefetch(std::size_t frames, const std::function<StereoSample()>& source) {
        state_.pending.erase(state_.pending.begin(),state_.pending.begin() + pending_);
        pending_ = 0;
        if (!state_.loaded) {
            if (frames == 0) { return; }
            state_.previous = state_.current;
            state_.current = source();
            --frames;
            state_.loaded = true;
            if (frames == 0) {
                state_.phase = *state_.interpolation_phase = device_audio_rate;
                return;
            }
            state_.previous = state_.current;
            state_.current = source();
            --frames;
            state_.phase = *state_.interpolation_phase = 0;
        }
        while (true) {
            if (state_.phase >= device_audio_rate) {
                if (frames == 0) { return; }
                state_.previous = state_.current;
                state_.current = source();
                --frames;
                state_.phase -= device_audio_rate;
                *state_.interpolation_phase = state_.phase;
            }
            while (*state_.interpolation_phase < device_audio_rate || state_.phase < device_audio_rate) {
                state_.pending.push_back(interpolate());
            }
        }
    }

    DeviceSample next(const std::function<StereoSample()>& source) {
        DeviceSample value{};
        if (pending_ < state_.pending.size()) {
            value = state_.pending[pending_++];
        } else {
            if (!state_.loaded) {
                state_.previous = source();
                state_.current = source();
                state_.loaded = true;
                state_.phase = *state_.interpolation_phase = 0;
            }
            auto& interpolation = *state_.interpolation_phase;
            while (state_.phase >= device_audio_rate && interpolation >= device_audio_rate) {
                state_.previous = state_.current;
                state_.current = source();
                state_.phase -= device_audio_rate;
                interpolation = state_.phase;
            }
            value = this->interpolate();
        }
        return filter(value);
    }

    DeviceSample silence() {
        DeviceSample value{};
        if (pending_ < state_.pending.size()) {
            value = state_.pending[pending_++];
        } else if (state_.loaded && *state_.interpolation_phase < device_audio_rate) {
            for (unsigned channel = 0; channel < 2; ++channel) {
                const auto delta = std::int64_t(state_.current[channel]) - state_.previous[channel];
                value[channel] = static_cast<std::int32_t>((state_.previous[channel] +
                    delta * *state_.interpolation_phase / device_audio_rate) * channel_gain);
            }
            state_.phase += rate_;
            *state_.interpolation_phase += rate_;
        } else if (state_.loaded && state_.phase < device_audio_rate) {
            for (unsigned channel = 0; channel < 2; ++channel) { value[channel] = state_.current[channel] * channel_gain; }
            state_.phase += rate_;
        } else {
            // The pinned mixer's exhausted-source padding holds raw DAC units.
            // It clamps consumption phase and retains interpolation history.
            std::copy(state_.current.begin(),state_.current.end(),value.begin());
            state_.phase = std::min(state_.phase,device_audio_rate);
        }
        return filter(value);
    }

private:
    DeviceSample interpolate() {
        DeviceSample value{};
        auto& phase = *state_.interpolation_phase;
        const bool interpolating = phase < device_audio_rate;
        for (unsigned channel = 0; channel < 2; ++channel) {
            const auto delta = std::int64_t(state_.current[channel]) - state_.previous[channel];
            const auto sample = interpolating ? state_.previous[channel] + delta * phase / device_audio_rate : state_.current[channel];
            value[channel] = static_cast<std::int32_t>(sample * channel_gain);
        }
        state_.phase += rate_;
        if (interpolating) { phase += rate_; }
        return value;
    }

    DeviceSample filter(DeviceSample value) {
        constexpr std::int32_t filter_unit = 65536;
        constexpr std::int32_t sb_filter_alpha = 27467;
        if (filter_ == DeviceFilter::none) { return value; }
        for (unsigned channel = 0; channel < 2; ++channel) {
            const auto sum = std::int64_t(value[channel]) * sb_filter_alpha +
                std::int64_t(state_.history[channel]) * (filter_unit - sb_filter_alpha);
            value[channel] = combat_math::shift(sum,16);
        }
        state_.history = value;
        return value;
    }

    static constexpr std::int32_t channel_gain = 8192;
    std::uint32_t rate_;
    DeviceFilter filter_;
    DeviceChannelState state_;
    std::size_t pending_ = 0;
};

inline StereoSample device_sum(const DeviceSample& effects, const DeviceSample& music) {
    StereoSample result{};
    for (unsigned channel = 0; channel < 2; ++channel) {
        const auto value = combat_math::shift(std::int64_t(effects[channel]) + music[channel],13);
        result[channel] = static_cast<std::int16_t>(std::clamp(value,-32768,32767));
    }
    return result;
}

}
