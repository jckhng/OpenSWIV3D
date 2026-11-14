#include "../src/audio.hpp"

#include <fstream>
#include <iostream>

namespace {
void pcm(const std::filesystem::path& path, const std::vector<std::int16_t>& samples) {
    std::ofstream output(path, std::ios::binary);
    for (auto sample : samples) {
        const auto value = static_cast<std::uint16_t>(sample);
        output.put(static_cast<char>(value)); output.put(static_cast<char>(value >> 8));
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 4) { throw std::runtime_error("Expected GAME CUE OUTPUT"); }
        const auto sounds = swiv3d::Archive::open(std::filesystem::path(argv[1]) / "SOUNDS.BD");
        const std::filesystem::path output(argv[3]);
        if (std::filesystem::exists(output)) { throw std::runtime_error("Audio output must be fresh"); }
        std::filesystem::create_directories(output);
        for (const auto& entry : sounds.resources()) {
            const auto sample = swiv3d::detail::decode_wave(sounds.load(entry.id));
            pcm(output / (std::to_string(entry.id) + ".pcm"), sample.samples);
        }
        swiv3d::detail::Audio audio(sounds);
        audio.present({{0,swiv3d::no_entity,5000,swiv3d::SoundAction::loop}}, {},
                      [](int) { return swiv3d::detail::SoundSource{}; });
        pcm(output / "engine.pcm", audio.mix(swiv3d::detail::audio_rate * 2));
        swiv3d::detail::CdAudio music(argv[2]);
        music.play(3);
        std::vector<std::int16_t> track(swiv3d::detail::audio_rate * 4);
        music.mix_into(track);
        pcm(output / "track3.pcm", track);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
