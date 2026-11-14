#include "cd_audio.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace swiv3d::detail {
namespace {
constexpr std::uint64_t sector_bytes = 2352, sectors_per_second = 75;
}

CdAudio::CdAudio(const std::filesystem::path& cue) {
    std::ifstream input(cue);
    if (!input) { throw std::runtime_error("Cannot read CD cue"); }
    std::string line, filename;
    bool file_seen = false;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string kind;
        row >> kind;
        if (kind == "FILE") {
            std::string format;
            if (file_seen || !(row >> std::quoted(filename) >> format) || format != "BINARY") {
                throw std::runtime_error("CD cue requires one binary image");
            }
            file_seen = true;
        } else if (kind == "TRACK") {
            unsigned number;
            std::string format;
            if (!(row >> number >> format) || (format != "AUDIO" && format != "MODE1/2352")) {
                throw std::runtime_error("Unsupported CD track");
            }
            tracks_.push_back({number, format == "AUDIO"});
        } else if (kind == "INDEX") {
            unsigned index, minutes, seconds, frames;
            char first, second;
            if (tracks_.empty() || !(row >> index >> minutes >> first >> seconds >> second >> frames) ||
                first != ':' || second != ':' || seconds >= 60 || frames >= sectors_per_second) {
                throw std::runtime_error("Invalid CD index");
            }
            if (index == 1) { tracks_.back().start = ((minutes * 60 + seconds) * sectors_per_second + frames) * sector_bytes; }
        }
    }
    if (!file_seen || tracks_.empty()) { throw std::runtime_error("Incomplete CD cue"); }
    const auto path = cue.parent_path() / filename;
    const auto size = std::filesystem::file_size(path);
    if (size % sector_bytes != 0) { throw std::runtime_error("CD image is not sector aligned"); }
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        auto& track = tracks_[i];
        track.end = i + 1 == tracks_.size() ? size : tracks_[i + 1].start;
        if (track.start >= track.end || track.end > size) { throw std::runtime_error("Invalid CD track bounds"); }
    }
    image_.open(path, std::ios::binary);
    if (!image_) { throw std::runtime_error("Cannot open CD image"); }
}

void CdAudio::play(unsigned track) {
    const auto found = std::find_if(tracks_.begin(), tracks_.end(), [track](const auto& entry) { return entry.number == track; });
    if (found == tracks_.end() || !found->audio) { throw std::runtime_error("Requested CD track is not audio"); }
    selected_ = track; position_ = found->start; end_ = found->end;
    image_.clear(); image_.seekg(static_cast<std::streamoff>(position_));
}

void CdAudio::poll() {
    if (selected_ != 0 && position_ == end_) { play(selected_); }
}

void CdAudio::stop() { selected_ = 0; }

std::array<std::int16_t,2> CdAudio::read_frame() {
    std::array<std::int16_t,2> result{};
    if (selected_ == 0 || position_ == end_) { return result; }
    std::array<unsigned char,4> bytes{};
    image_.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
    if (image_.gcount() != static_cast<std::streamsize>(bytes.size())) { throw std::runtime_error("Truncated CD audio"); }
    position_ += bytes.size();
    for (unsigned channel = 0; channel < 2; ++channel) {
        result[channel] = static_cast<std::int16_t>(unsigned(bytes[channel*2]) | unsigned(bytes[channel*2+1]) << 8);
    }
    return result;
}

void CdAudio::mix_into(std::vector<std::int16_t>& stereo) {
    if (selected_ == 0 || position_ == end_) { return; }
    const auto count = std::min<std::uint64_t>(stereo.size() * 2, end_ - position_);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
    image_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
    if (static_cast<std::uint64_t>(image_.gcount()) != count) { throw std::runtime_error("Truncated CD audio"); }
    position_ += count;
    for (std::size_t i = 0; i < bytes.size() / 2; ++i) {
        const auto value = static_cast<std::int16_t>(unsigned(bytes[i * 2]) | unsigned(bytes[i * 2 + 1]) << 8);
        stereo[i] = static_cast<std::int16_t>(std::clamp(int(stereo[i]) + value, -32768, 32767));
    }
}
}
