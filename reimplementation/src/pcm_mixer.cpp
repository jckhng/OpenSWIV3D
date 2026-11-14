#include "audio.hpp"
#include "combat_math.hpp"
#include "digital_work.hpp"

#include <algorithm>

namespace swiv3d::detail {
namespace {
constexpr std::uint64_t phase_unit = UINT64_C(1) << 16;
constexpr int unity_tolerance = 655;
constexpr int active = 1, looping = 2;
}

std::uint64_t merge_pcm(const PcmSample& sample, SoundVoice& voice, std::vector<std::int32_t>& accumulation, int master_volume, PcmBuffering buffering, PcmMergeProgress* progress) {
    const int volume = std::clamp(voice.volume,0,127) * std::clamp(master_volume,0,127) / 127;
    const auto count = sample.samples.size() / sample.channels;
    auto step = static_cast<std::uint64_t>(std::max(1, voice.rate)) * phase_unit / digital_rate;
    if (step >= phase_unit - unity_tolerance && step <= phase_unit + unity_tolerance) { step = phase_unit; }
    // Each Miles merge starts at half a source sample; fractional phase is
    // discarded at buffer and loop boundaries.
    auto position = voice.position + phase_unit / 2;
    const auto costs = digital_work::merge(sample.bits,sample.channels,voice.rate,voice.volume,voice.pan,master_volume);
    const bool looping_voice = (voice.flags & looping) != 0;
    const bool streamed = buffering == PcmBuffering::streamed;
    constexpr unsigned stream_buffer_bytes = 12288;
    const auto buffer_frames = progress && progress->boundary_frames != 0 ? progress->boundary_frames :
        stream_buffer_bytes / (sample.channels * sample.bits / 8);
    auto boundary = (position / phase_unit / buffer_frames + 1) * buffer_frames;
    unsigned segments = 0, crossings = 0, rendered = 0, source_exits = 0;
    bool segment = false, exhausted = false;
    if (streamed && voice.position != 0 && voice.position / phase_unit < count && voice.position / phase_unit % buffer_frames == 0) { ++crossings; }
    for (std::size_t frame = 0; frame < accumulation.size() / 2 && (voice.flags & active) != 0; ++frame) {
        auto index = static_cast<std::size_t>(position / phase_unit);
        if (index >= count) {
            if (!looping_voice || count == 0) {
                if (!streamed) { voice.flags &= ~active; }
                exhausted = true;
                if (segment) { ++source_exits; }
                break;
            }
            if (segment) { ++source_exits; }
            ++crossings;
            segment = false;
            position = phase_unit / 2;
            index = 0;
        }
        if (streamed && index >= boundary) {
            if (segment) { ++source_exits; }
            ++crossings;
            segment = false;
            boundary += buffer_frames;
            position = (boundary - buffer_frames) * phase_unit + phase_unit / 2;
            index = static_cast<std::size_t>(position / phase_unit);
        }
        if (!segment) { ++segments; segment = true; }
        if (progress) { progress->last_source = index; }
        ++rendered;
        for (int channel = 0; channel < 2; ++channel) {
            const int pan = channel == 0 ? 127 - voice.pan : voice.pan;
            const int gain = pan < 63 ? pan * 2 : 128;
            const int value = sample.samples[index * sample.channels + (sample.channels == 1 ? 0 : channel)];
            accumulation[frame * 2 + channel] += sample.bits == 8 ?
                combat_math::shift(combat_math::shift(value * (volume == 0 ? 0 : volume + 1), 7) * gain, 7) :
                combat_math::shift(value * (gain * volume / 127), 7);
        }
        position += step;
    }
    voice.position = position / phase_unit * phase_unit;
    return 20 + std::uint64_t(segments) * ((looping_voice ? 26 : 23) + costs.setup + 1) +
        std::uint64_t(rendered) * costs.frame + source_exits * 2 +
        crossings * (streamed ? 32 : 29) + (exhausted ? (streamed ? 29 : 22) : 0);
}

std::int16_t digital_sample(std::int32_t value) {
    return static_cast<std::int16_t>(combat_math::shift(std::clamp(value, -32768, 32767), 8) * 256);
}
}
