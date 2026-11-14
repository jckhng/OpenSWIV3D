#include "audio.hpp"

#include <algorithm>
#include <stdexcept>

namespace swiv3d::detail {
namespace {
constexpr std::uint64_t source_phase_unit = UINT64_C(1) << 16;
constexpr unsigned cache_alignment = 4096, sample_alignment = 256;

std::uint32_t aligned(std::uint64_t value, unsigned alignment) {
    return static_cast<std::uint32_t>((value + alignment - 1) / alignment * alignment);
}
}

void MovieSound::open(PcmSample sample, std::uint32_t duration) {
    if (sample.rate == 0 || (sample.channels != 1 && sample.channels != 2) ||
        (sample.bits != 8 && sample.bits != 16) || sample.samples.size() % sample.channels != 0) {
        throw std::runtime_error("Unsupported movie PCM format");
    }
    state_ = {};
    open_ = true;
    read_ = write_ = 0;
    const auto bytes_per_second = std::uint64_t(sample.rate) * sample.channels * sample.bits / 8;
    const auto frame_step = bytes_per_second * duration * 128 / 1000000;
    // MSS reserves one second plus two frame deadlines in the decoded ring.
    const auto capacity = std::max(8192u,aligned(bytes_per_second + (frame_step >> 6),cache_alignment));
    constexpr unsigned rate_tolerance = 655, tolerance_scale = 32768;
    const auto block_bytes = std::uint64_t(SoundBlasterDma::block_frames) * bytes_per_second / digital_rate;
    const auto rate_slack = block_bytes * rate_tolerance / tolerance_scale;
    const bool exact_ratio = sample.rate == digital_rate || sample.rate == digital_rate * 2 || sample.rate == digital_rate * 4;
    minimum_bytes_ = aligned(block_bytes + rate_slack + (exact_ratio ? 0 : 4),sample_alignment);
    buffer_bytes_ = std::max(minimum_bytes_,aligned(bytes_per_second >> 2,cache_alignment));
    ring_.assign(capacity / (sample.bits / 8),0);
    for (auto& buffer : buffers_) { buffer.clear(); }
    auto samples = std::move(sample.samples);
    format_ = std::move(sample);
    append(samples);
}

void MovieSound::append(const std::vector<std::int16_t>& samples) {
    if (!open_ || format_.channels == 0 || samples.size() % format_.channels != 0) {
        throw std::runtime_error("Invalid movie PCM continuation");
    }
    state_.ended = samples.empty();
    const auto bytes = format_.bits / 8;
    const auto count = std::min(samples.size(),ring_.size() - state_.available / bytes);
    for (std::size_t sample = 0; sample < count; ++sample) {
        ring_[write_] = samples[sample];
        write_ = (write_ + 1) % ring_.size();
    }
    state_.available += static_cast<std::uint32_t>(count * bytes);
    state_.decoded += static_cast<std::uint32_t>(count * bytes);
}

bool MovieSound::service(std::uint32_t milliseconds) {
    if (!open_) { return false; }
    if (state_.available <= minimum_bytes_ && (!state_.ended || state_.available <= 3)) { return false; }
    unsigned buffer = 0;
    if (state_.last_ready == UINT32_MAX - 1) {
        state_.last_ready = UINT32_MAX;
    } else if (state_.last_ready == UINT32_MAX) {
        state_.last_ready = state_.current;
        buffer = 1;
    } else {
        if (state_.last_ready == state_.current) { return false; }
        state_.last_ready = state_.current;
        buffer = state_.current ^ 1;
    }
    const auto bytes = std::min(state_.available & ~3u,buffer_bytes_ & ~3u);
    auto& destination = buffers_[buffer];
    destination.resize(bytes / (format_.bits / 8));
    for (auto& sample : destination) {
        sample = ring_[read_];
        read_ = (read_ + 1) % ring_.size();
    }
    if (!state_.epoch || !state_.playing) {
        state_.epoch = milliseconds;
        state_.completed += state_.queued;
        state_.queued = 0;
    }
    state_.lengths[buffer] = bytes;
    state_.positions[buffer] = 0;
    state_.playing = true;
    state_.queued += bytes;
    state_.available -= bytes;
    return true;
}

std::uint32_t MovieSound::played(std::uint32_t milliseconds) {
    auto bytes = state_.queued;
    if (state_.epoch) {
        const auto rate = std::uint64_t(format_.rate) * format_.channels * format_.bits / 8;
        bytes = static_cast<std::uint32_t>(rate * std::uint32_t(milliseconds - *state_.epoch) / 1000);
    }
    if (bytes > state_.queued) {
        state_.epoch.reset();
        bytes = state_.queued;
    }
    return state_.completed + bytes;
}

std::uint64_t MovieSound::merge(std::vector<std::int32_t>& accumulation, int master_volume) {
    if (!state_.playing) { return 0; }
    const auto current = state_.current;
    const auto next = current ^ 1;
    const auto sample_bytes = format_.bits / 8;
    const auto frame_bytes = sample_bytes * format_.channels;
    const auto boundary = state_.lengths[current] / frame_bytes;
    auto sample = format_;
    sample.samples = buffers_[current];
    const bool queued_next = state_.lengths[next] != 0 && state_.positions[next] == 0;
    if (queued_next) { sample.samples.insert(sample.samples.end(),buffers_[next].begin(),buffers_[next].end()); }
    SoundVoice voice{0,no_entity,1,127,64,static_cast<int>(format_.rate),
        std::uint64_t(state_.positions[current] / frame_bytes) * source_phase_unit};
    PcmMergeProgress progress{{},boundary};
    const auto work = merge_pcm(sample,voice,accumulation,master_volume,PcmBuffering::streamed,&progress);
    const auto position = voice.position / source_phase_unit * frame_bytes;
    state_.positions[current] = static_cast<std::uint32_t>(std::min<std::uint64_t>(position,state_.lengths[current]));
    if (queued_next && progress.last_source && *progress.last_source >= boundary) {
        state_.current = next;
        state_.positions[next] = static_cast<std::uint32_t>(position - state_.lengths[current]);
    }
    return work;
}

void MovieSound::close() { open_ = false; state_.playing = false; }

}
