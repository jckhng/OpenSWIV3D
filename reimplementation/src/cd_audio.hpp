#pragma once

#include <cstdint>
#include <array>
#include <filesystem>
#include <fstream>
#include <vector>

namespace swiv3d::detail {
class CdAudio {
public:
    explicit CdAudio(const std::filesystem::path& cue);
    void play(unsigned track);
    void stop();
    void poll();
    void mix_into(std::vector<std::int16_t>& stereo);
    std::array<std::int16_t,2> read_frame();

private:
    struct Track { unsigned number; bool audio; std::uint64_t start = 0, end = 0; };
    std::ifstream image_;
    std::vector<Track> tracks_;
    unsigned selected_ = 0;
    std::uint64_t position_ = 0, end_ = 0;
};
}
