#include "audio.hpp"
#include "combat_math.hpp"
#include "digital_work.hpp"
#include "movie_preparation.hpp"
#include "sb_startup.hpp"

#include <algorithm>
#include <stdexcept>

namespace swiv3d::detail {
namespace {
constexpr int active = 1, looping = 2, pending = 4;
constexpr std::size_t digital_buffer_frames = SoundBlasterDma::block_frames;

unsigned little16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return bytes.at(offset) | unsigned(bytes.at(offset + 1)) << 8;
}

bool tag(const std::vector<std::uint8_t>& bytes, std::size_t offset, const char* text) {
    return offset + 4 <= bytes.size() && std::equal(text, text + 4, bytes.begin() + offset);
}

std::uint64_t digital_service_work(SoundBlasterDma::Service service, std::uint64_t refill) {
    if (service == SoundBlasterDma::Service::idle) { return digital_work::idle_service; }
    if (service == SoundBlasterDma::Service::retained) { return digital_work::retained_service; }
    if (service == SoundBlasterDma::Service::stopped) {
        return refill + digital_work::stop_caller + digital_work::driver_stop;
    }
    return refill;
}
}

PcmSample decode_wave(const std::vector<std::uint8_t>& bytes) {
    if (!tag(bytes, 0, "RIFF") || !tag(bytes, 8, "WAVE")) { throw std::runtime_error("Sound is not a RIFF WAVE"); }
    PcmSample result{};
    unsigned bits = 0;
    std::size_t data = 0, size = 0;
    for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
        const auto count = static_cast<std::uint32_t>(combat_math::word(bytes, offset + 4));
        if (count > bytes.size() - offset - 8) { throw std::runtime_error("Truncated sound chunk"); }
        if (tag(bytes, offset, "fmt ")) {
            if (count < 16 || little16(bytes, offset + 8) != 1) { throw std::runtime_error("Sound needs PCM format"); }
            result.channels = static_cast<std::uint16_t>(little16(bytes, offset + 10));
            result.rate = static_cast<std::uint32_t>(combat_math::word(bytes, offset + 12));
            bits = little16(bytes, offset + 22);
        }
        if (tag(bytes, offset, "data")) { data = offset + 8; size = count; }
        offset += 8 + count + (count & 1);
    }
    if (data == 0 || result.rate == 0 || (result.channels != 1 && result.channels != 2) ||
        (bits != 8 && bits != 16) || size % (result.channels * bits / 8) != 0) {
        throw std::runtime_error("Unsupported sound dimensions");
    }
    result.bits = bits;
    for (std::size_t offset = data; offset < data + size; offset += bits / 8) {
        const int sample = bits == 8 ? (int(bytes[offset]) - 128) * 256 :
                           static_cast<std::int16_t>(little16(bytes, offset));
        result.samples.push_back(static_cast<std::int16_t>(sample));
    }
    return result;
}

Audio::Audio(const Archive& sounds) {
    for (const auto& entry : sounds.resources()) {
        samples_.emplace(entry.id / 100, decode_wave(sounds.load(entry.id)));
    }
}

void Audio::stop() {
    for (auto& voice : voices_) { voice.flags &= ~active; }
}

void Audio::open_music(const std::filesystem::path& cue) {
    music_ = std::make_unique<CdAudio>(cue);
    if (track_ != 0) { music_->play(track_); }
}

void Audio::music_track(unsigned track) {
    track_ = track;
    if (music_) { music_->play(track); }
}

void Audio::stop_music() {
    track_ = 0;
    if (music_) { music_->stop(); }
    music_device_.disable();
}

void Audio::begin_movie(PcmSample sample, std::uint32_t duration, std::optional<std::uint32_t> epoch) {
    movie_.open(std::move(sample),duration);
    movie_timer_start_ = epoch.value_or(static_cast<std::uint32_t>(device_dma_.timer_milliseconds()));
    if (movie_.service(static_cast<std::uint32_t>(movie_timer_start_))) { device_dma_.start(); }
}

void Audio::elapse_initialization(std::uint64_t cycles) {
    device_dma_.elapse_cycles(cycles,[](auto&) { return false; },[&](auto service) {
        irq_work_.push_back(timer_service_.interrupt() + digital_service_work(service,0));
    });
    device_cycles_ += cycles;
}

std::uint64_t Audio::begin_opening(PcmSample sample, std::uint32_t duration, const MoviePreparation& preparation) {
    if (device_cycles_ != 0) { throw std::runtime_error("Opening initialization requires a fresh audio device"); }
    constexpr unsigned timestamp_tail_work = 3965;
    const auto& stages = preparation.stages();
    if (stages.empty() || stages.back().kind != PreparationKind::cpu || stages.back().work < timestamp_tail_work) {
        throw std::runtime_error("Opening lacks its first sound-service timestamp");
    }
    MovieSound prepared;
    prepared.open(std::move(sample),duration);
    constexpr unsigned startup_messages = 3000 / 25;
    constexpr unsigned wait_return_work = 35;
    constexpr auto pit_ticks = std::uint64_t(CompatibilityClock::pit_period_ticks) *
        CompatibilityClock::interrupts_per_update * startup_messages;
    constexpr auto irq_delay = CompatibilityClock::cpu_frequency * (1000000000 / CompatibilityClock::pit_frequency) / 1000000000;
    constexpr auto wait_cycle = (pit_ticks * CompatibilityClock::cpu_frequency + CompatibilityClock::pit_frequency - 1) /
        CompatibilityClock::pit_frequency + irq_delay;
    elapse_initialization(wait_cycle);
    RasterPhase phase;
    phase.dac_ms = phase.dac_period_ms = 1000.0 / (digital_rate * 2);
    RasterSchedule schedule(phase,{},IrqScheduling::device,[this](auto interrupt) { return irq_work(interrupt); });
    const auto advance = [&](std::uint64_t end) { elapse_initialization(end - device_cycles_); };
    const auto cpu = [&](std::uint64_t work) { advance(schedule.cpu(device_cycles_,work)); };
    cpu(wait_return_work);
    opening_clock_.emplace();
    opening_clock_->wait_return = device_cycles_;
    std::optional<std::uint32_t> epoch;
    for (std::size_t index = 0; index < stages.size(); ++index) {
        const auto& stage = stages[index];
        if (stage.kind == PreparationKind::disk) {
            advance(schedule.idle(device_cycles_,stage.work));
        } else if (index + 1 != stages.size()) { cpu(stage.work); }
        else {
            cpu(stage.work - timestamp_tail_work);
            epoch = movie_timestamp(0);
            opening_clock_->first_timestamp = device_cycles_;
            cpu(timestamp_tail_work);
        }
    }
    if (!epoch) { throw std::runtime_error("Opening lacks first-frame preparation"); }
    movie_ = std::move(prepared);
    movie_timer_start_ = *epoch;
    movie_.service(*epoch);
    // Rate command, rate bytes, DMA command, mode and block-length bytes.
    opening_clock_->driver_entry = device_cycles_;
    constexpr std::array<unsigned,7> command_work{883,22,22,29,22,22,22};
    constexpr unsigned dma_unmask_work = 818;
    constexpr unsigned status_read_work = 62, status_branch_work = 2, retry_work = 3;
    constexpr unsigned byte_write_work = 46, driver_return_work = 297;
    DspBusyClock dsp;
    for (std::size_t command = 0; command < command_work.size(); ++command) {
        if (command == 0) {
            cpu(dma_unmask_work);
            advance(schedule.instruction(device_cycles_,byte_write_work,[&](auto complete) {
                advance(complete);
                schedule.stop_dac_clock();
            }));
            cpu(command_work[command] - dma_unmask_work - byte_write_work);
        } else { cpu(command_work[command]); }
        while (true) {
            bool busy = false;
            advance(schedule.instruction(device_cycles_,status_read_work,[&](auto complete) {
                advance(complete);
                busy = dsp.busy(complete);
            }));
            cpu(status_branch_work);
            if (!busy) { break; }
            cpu(retry_work);
        }
        cpu(1); // Restore the command byte before OUT.
        const auto final_command = command + 1 == command_work.size();
        const auto end = schedule.instruction(device_cycles_,byte_write_work,[&](auto complete) {
            advance(complete);
            schedule.io_completion(dsp.write(complete),[&] { dsp.complete(); });
            if (final_command) {
                device_dma_.start();
                opening_clock_->activation = complete;
            }
        });
        if (final_command) { return end - device_cycles_ + driver_return_work; }
        advance(end);
    }
    throw std::runtime_error("Opening driver did not activate");
}

void Audio::append_movie(const std::vector<std::int16_t>& samples) {
    service_movie();
    movie_.append(samples);
    service_movie();
}

void Audio::end_movie() { movie_.close(); }

void Audio::service_movie() {
    if (movie_.service(static_cast<std::uint32_t>(device_dma_.timer_milliseconds()))) { device_dma_.start(); }
}

std::uint32_t Audio::movie_bytes_played() {
    service_movie();
    return movie_.played(static_cast<std::uint32_t>(device_dma_.timer_milliseconds()));
}

std::uint32_t Audio::movie_timestamp(std::uint64_t cycles_before) const {
    return static_cast<std::uint32_t>(device_dma_.timer_milliseconds(cycles_before));
}

void Audio::poll_music() { if (music_) { music_->poll(); } }
void Audio::effects_volume(int volume) { effects_volume_ = std::clamp(volume,0,255); }

const std::array<SoundVoice, sound_channels>& Audio::voices() const { return voices_; }

void Audio::enqueue(const std::vector<SoundEvent>& events) {
    for (const auto& event : events) {
        if (event.action == SoundAction::stop_all) { stop(); continue; }
        if (event.action == SoundAction::stop) {
            for (auto& voice : voices_) {
                if (voice.actor == event.actor && voice.sound == event.sound) { voice.flags &= ~active; }
            }
            continue;
        }
        const auto sample = samples_.find(event.sound);
        if (sample == samples_.end()) { continue; }
        auto channel = std::find_if(voices_.begin(), voices_.end(), [](const auto& voice) { return (voice.flags & active) == 0; });
        if (channel == voices_.end()) {
            int quietest = 128;
            for (auto candidate = voices_.begin(); candidate != voices_.end(); ++candidate) {
                if ((candidate->flags & (pending | looping)) == 0 && candidate->volume < quietest) {
                    channel = candidate; quietest = candidate->volume;
                }
            }
        }
        if (channel == voices_.end()) { continue; }
        *channel = {event.sound, event.actor, active | pending | (event.action == SoundAction::loop ? looping : 0),
                    event.actor == no_entity ? 100 : 127, 64, static_cast<int>(sample->second.rate), 0};
        device_dma_.start();
    }
}

void Audio::present(const std::vector<SoundEvent>& events, const SoundListener& listener,
                    const std::function<SoundSource(int)>& source) {
    enqueue(events);
    for (auto& voice : voices_) {
        if ((voice.flags & active) == 0) { continue; }
        voice.volume = 100;
        if (voice.actor != no_entity) {
            const auto actor = source(voice.actor);
            std::array<int, 3> delta{};
            // OS_SOUND_Render_sound precedes this frame's camera update.
            for (int axis = 0; axis < 3; ++axis) { delta[axis] = actor.position[axis] - listener_.position[axis]; }
            for (int axis : {0, 2}) {
                if (delta[axis] > 10000) { delta[axis] -= 32768; }
                if (delta[axis] < -10000) { delta[axis] += axis == 2 ? 32758 : 32768; }
            }
            int depth = 0, side = 0;
            for (int axis = 0; axis < 3; ++axis) {
                depth += delta[axis] * listener_.forward[axis];
                side += delta[axis] * listener_.right[axis];
            }
            depth = std::max(5, std::abs(combat_math::shift(depth, 7)));
            voice.pan = std::clamp(side / (depth + 80), -64, 63) + 64;
            voice.volume = std::min(127, 20000 / (depth + 80));
            if ((voice.flags & looping) != 0) { voice.rate = std::max(0, actor.rate); }
        }
        voice.flags &= ~pending;
    }
    listener_ = listener;
}

bool Audio::build_digital_buffer() {
    return build_digital_buffer(voices_,movie_,digital_buffer_,digital_work_);
}

bool Audio::build_digital_buffer(std::array<SoundVoice,sound_channels>& voices, MovieSound& movie,
                                std::vector<std::int32_t>& buffer, std::uint64_t& work) const {
    buffer.assign(digital_buffer_frames * 2, 0);
    bool playing = false;
    unsigned active_count = 0;
    work = 0;
    for (auto& voice : voices) {
        if ((voice.flags & active) == 0) { continue; }
        playing = true;
        ++active_count;
        work += merge_pcm(samples_.at(voice.sound), voice, buffer,effects_volume_ >> 1);
    }
    if (movie.state().playing) {
        playing = true;
        ++active_count;
        work += movie.merge(buffer,effects_volume_ >> 1);
    }
    work += digital_work::refill(active_count,buffer);
    return playing;
}

std::uint64_t Audio::irq_work(std::uint64_t interrupt) const {
    if (interrupt == 0) { throw std::runtime_error("IRQ ordinals start at one"); }
    if (interrupt <= irq_work_.size()) { return irq_work_[static_cast<std::size_t>(interrupt - 1)]; }
    auto dma = device_dma_;
    auto effects = effects_device_;
    auto timer = timer_service_;
    auto voices = voices_;
    auto movie = movie_;
    std::vector<std::int32_t> buffer;
    std::uint64_t work = 0, ordinal = irq_work_.size();
    const auto produce = [&](auto&) { return build_digital_buffer(voices,movie,buffer,work); };
    // Forecast only digital state. CD reads and live source cursors retain
    // their ownership in mix_device.
    const auto source = [&] { return dma.next(); };
    while (true) {
        const auto service = dma.elapse_frame(produce);
        if (service != SoundBlasterDma::Service::none) {
            const auto total = timer.interrupt() + digital_service_work(service,work);
            if (++ordinal == interrupt) { return total; }
        }
        if (dma.playing()) { effects.next(source); }
        else { effects.silence(); }
    }
}

StereoSample Audio::digital_frame() {
    if (digital_offset_ == digital_buffer_.size()) {
        build_digital_buffer();
        digital_offset_ = 0;
    }
    StereoSample result{};
    for (auto& sample : result) { sample = digital_sample(digital_buffer_[digital_offset_++]); }
    return result;
}

std::vector<std::int16_t> Audio::mix(std::size_t frames) {
    std::vector<std::int16_t> output(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        if (digital_phase_ == 0) { digital_output_ = digital_frame(); }
        for (int channel = 0; channel < 2; ++channel) { output[frame * 2 + channel] = digital_output_[channel]; }
        digital_phase_ = (digital_phase_ + 1) % (audio_rate / digital_rate);
    }
    if (music_) { music_->mix_into(output); }
    return output;
}

std::uint64_t Audio::movie_milliseconds() const { return device_dma_.timer_milliseconds() - movie_timer_start_; }

std::uint64_t Audio::cpu_work() const { return cpu_work_; }

std::uint64_t Audio::device_cycles() const {
    return device_cycles_;
}

std::vector<std::int16_t> Audio::mix_device(std::size_t frames) {
    cpu_work_ = 0;
    std::vector<std::int16_t> output(frames * 2);
    const auto digital = [this] { return device_dma_.next(); };
    const auto produce = [this](SoundBlasterDma::Block& block) {
        const auto playing = build_digital_buffer();
        for (std::size_t frame = 0; frame < block.size(); ++frame) {
            for (unsigned channel = 0; channel < 2; ++channel) {
                block[frame][channel] = digital_sample(digital_buffer_[frame*2+channel]);
            }
        }
        return playing;
    };
    const auto music = [this] { return music_ ? music_->read_frame() : StereoSample{}; };
    for (std::size_t frame = 0; frame < frames; ++frame) {
        device_cycles_ += CompatibilityClock::cpu_frequency / device_audio_rate;
        const auto service = device_dma_.elapse_frame(produce);
        if (service != SoundBlasterDma::Service::none) {
            const auto work = timer_service_.interrupt() + digital_service_work(service,digital_work_);
            irq_work_.push_back(work);
            irq_debt_ += work;
        }
        constexpr auto work_per_sample = CompatibilityClock::cpu_frequency / device_audio_rate;
        const auto serviced = std::min(irq_debt_,work_per_sample);
        irq_debt_ -= serviced;
        cpu_work_ += work_per_sample - serviced;
        const auto music_sample = music_ && track_ != 0 ? music_device_.next(music) : DeviceSample{};
        const auto effects_sample = device_dma_.playing() ? effects_device_.next(digital) : effects_device_.silence();
        const auto sample = device_sum(effects_sample,music_sample);
        std::copy(sample.begin(),sample.end(),output.begin()+frame*2);
    }
    return output;
}

}
