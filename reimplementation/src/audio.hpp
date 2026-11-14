#pragma once

#include "swiv3d/mission.hpp"
#include "cd_audio.hpp"
#include "device_audio.hpp"
#include "sb_dma.hpp"
#include "timer_service.hpp"

#include <functional>
#include <map>

namespace swiv3d::detail {

class MoviePreparation;

constexpr std::uint32_t audio_rate = 44100;
constexpr std::uint32_t digital_rate = 11025;
constexpr std::size_t sound_channels = 13;

struct PcmSample {
    std::uint32_t rate;
    std::uint16_t channels;
    std::vector<std::int16_t> samples;
    unsigned bits = 8;
};

PcmSample decode_wave(const std::vector<std::uint8_t>& bytes);

struct SoundSource {
    std::array<int, 3> position{};
    int rate = 11025;
};

struct SoundListener {
    std::array<int, 3> position{}, right{}, forward{};
};

struct SoundVoice {
    int sound = 0, actor = no_entity, flags = 0, volume = 127, pan = 64, rate = 11025;
    std::uint64_t position = 0;
};

enum class PcmBuffering { single, streamed };

struct PcmMergeProgress {
    std::optional<std::size_t> last_source;
    std::size_t boundary_frames = 0;
};

std::uint64_t merge_pcm(const PcmSample& sample, SoundVoice& voice, std::vector<std::int32_t>& accumulation, int master_volume = 127, PcmBuffering buffering = PcmBuffering::single, PcmMergeProgress* progress = nullptr);
std::int16_t digital_sample(std::int32_t value);

class MovieSound {
public:
    struct State {
        std::uint32_t available = 0, queued = 0, completed = 0, decoded = 0;
        std::optional<std::uint32_t> epoch;
        std::array<std::uint32_t,2> lengths{}, positions{};
        unsigned current = 0;
        std::uint32_t last_ready = UINT32_MAX - 1;
        bool playing = false, ended = false;
    };

    void open(PcmSample sample, std::uint32_t duration);
    void append(const std::vector<std::int16_t>& samples);
    bool service(std::uint32_t milliseconds);
    std::uint32_t played(std::uint32_t milliseconds);
    std::uint64_t merge(std::vector<std::int32_t>& accumulation, int master_volume);
    void close();
    const State& state() const { return state_; }

private:
    State state_;
    PcmSample format_{};
    std::vector<std::int16_t> ring_;
    std::array<std::vector<std::int16_t>,2> buffers_;
    std::size_t read_ = 0, write_ = 0;
    std::uint32_t buffer_bytes_ = 0, minimum_bytes_ = 0;
    bool open_ = false;
};

class Audio {
public:
    struct OpeningClock {
        std::uint64_t wait_return = 0, first_timestamp = 0, driver_entry = 0, activation = 0;
    };
    explicit Audio(const Archive& sounds);
    void enqueue(const std::vector<SoundEvent>& events);
    void present(const std::vector<SoundEvent>& events, const SoundListener& listener,
                 const std::function<SoundSource(int)>& source);
    std::vector<std::int16_t> mix(std::size_t frames);
    std::vector<std::int16_t> mix_device(std::size_t frames);
    std::uint64_t cpu_work() const;
    std::uint64_t movie_milliseconds() const;
    std::uint32_t movie_bytes_played();
    const MovieSound::State& movie_sound_state() const { return movie_.state(); }
    std::uint32_t movie_timestamp(std::uint64_t cycles_before) const;
    void service_movie();
    std::uint64_t irq_work(std::uint64_t interrupt) const;
    std::uint64_t device_cycles() const;
    const std::optional<OpeningClock>& opening_clock() const { return opening_clock_; }
    void stop();
    void open_music(const std::filesystem::path& cue);
    void music_track(unsigned track);
    void stop_music();
    void begin_movie(PcmSample sample, std::uint32_t duration = 0, std::optional<std::uint32_t> epoch = {});
    std::uint64_t begin_opening(PcmSample sample, std::uint32_t duration, const MoviePreparation& preparation);
    void append_movie(const std::vector<std::int16_t>& samples);
    void end_movie();
    void poll_music();
    void effects_volume(int volume);
    const std::array<SoundVoice, sound_channels>& voices() const;

private:
    bool build_digital_buffer();
    bool build_digital_buffer(std::array<SoundVoice,sound_channels>& voices, MovieSound& movie,
                              std::vector<std::int32_t>& buffer, std::uint64_t& work) const;
    StereoSample digital_frame();
    void elapse_initialization(std::uint64_t cycles);
    std::map<int, PcmSample> samples_;
    std::array<SoundVoice, sound_channels> voices_{};
    SoundListener listener_;
    std::unique_ptr<CdAudio> music_;
    unsigned track_ = 0;
    unsigned digital_phase_ = 0;
    int effects_volume_ = 255;
    std::array<std::int16_t, 2> digital_output_{};
    std::vector<std::int32_t> digital_buffer_;
    std::size_t digital_offset_ = 0;
    DeviceChannel effects_device_ = DeviceChannel::sound_blaster(digital_rate);
    DeviceChannel music_device_{audio_rate,DeviceFilter::none};
    SoundBlasterDma device_dma_;
    TimerService timer_service_;
    std::uint64_t digital_work_ = 0, cpu_work_ = 0, irq_debt_ = 0, movie_timer_start_ = 0;
    std::vector<std::uint64_t> irq_work_;
    std::uint64_t device_cycles_ = 0;
    MovieSound movie_;
    std::optional<OpeningClock> opening_clock_;
};

}
