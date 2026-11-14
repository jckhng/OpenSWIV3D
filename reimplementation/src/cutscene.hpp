#pragma once

#include "swiv3d/mission.hpp"
#include "movie_preparation.hpp"

namespace swiv3d::detail {

class Audio;
enum class MoviePolling { enabled, suspended };

class Cutscene {
public:
    Cutscene(const std::filesystem::path& directory, const std::string& name,
             const Archive& globals, const FrontendState& request);
    ~Cutscene();
    bool advance(std::uint32_t microseconds);
    bool advance_device(std::size_t frames, MoviePolling polling = MoviePolling::enabled);
    std::size_t device_frames_until_poll() const;
    MissionFrame render() const;
    std::vector<std::int16_t> mix(std::size_t frames);
    std::vector<std::int16_t> mix_device(std::size_t frames);
    void attach_audio(Audio& audio);
    void initialize_audio(Audio& audio);
    void prepare_audio_clock(Audio& audio, std::uint64_t before, std::uint64_t after, std::uint64_t cpu_work);
    std::uint32_t frame() const;
    std::uint32_t frames() const;
    std::uint32_t frame_duration() const;
    std::uint64_t preparation_work() const;
    MoviePreparation preparation() const;
    std::uint64_t first_video_work() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}
