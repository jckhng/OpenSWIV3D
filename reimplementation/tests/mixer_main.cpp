#include "../src/audio.hpp"
#include "../src/digital_work.hpp"

#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    try {
        if (argc == 5 && (std::string(argv[1]) == "--work-cases" || std::string(argv[1]) == "--work-multiple")) {
            const bool multiple = std::string(argv[1]) == "--work-multiple";
            std::ifstream wav(argv[2],std::ios::binary);
            const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(wav),{}};
            const auto sample = swiv3d::detail::decode_wave(bytes);
            std::ifstream cases(argv[3]);
            std::ofstream output(argv[4]);
            int rate, volume, pan, looping, frames, master, buffered;
            std::uint64_t position;
            while (cases >> rate >> volume >> pan >> looping >> position >> frames >> master >> buffered) {
                unsigned voices = 1;
                if (multiple && !(cases >> voices)) { throw std::runtime_error("Missing refill voice count"); }
                std::vector<std::int32_t> accumulation(frames * 2);
                std::uint64_t work = 0;
                for (unsigned slot = 0; slot < voices; ++slot) {
                    swiv3d::detail::SoundVoice voice{0,swiv3d::no_entity,1 | (looping ? 2 : 0),volume,pan,rate,position * 65536};
                    work += swiv3d::detail::merge_pcm(sample,voice,accumulation,master,
                        buffered ? swiv3d::detail::PcmBuffering::streamed : swiv3d::detail::PcmBuffering::single);
                }
                output << swiv3d::detail::digital_work::refill(voices,accumulation) + work << '\n';
            }
            if (!cases.eof() || !output) { throw std::runtime_error("Invalid refill work fixture"); }
            return 0;
        }
        if (argc != 4) { throw std::runtime_error("Expected WAVE CASES OUTPUT"); }
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), {}};
        const auto sample = swiv3d::detail::decode_wave(bytes);
        std::ifstream cases(argv[2]);
        std::ofstream output(argv[3]);
        if (!cases || !output) { throw std::runtime_error("Cannot open mixer fixture"); }
        int rate, volume, pan, loop, voices, frames;
        while (cases >> rate >> volume >> pan >> loop >> voices >> frames) {
            std::vector<std::int32_t> accumulation(frames * 2);
            swiv3d::detail::SoundVoice voice;
            for (int i = 0; i < voices; ++i) {
                voice = {0,swiv3d::no_entity,1 | (loop ? 2 : 0),volume,pan,rate,0};
                swiv3d::detail::merge_pcm(sample, voice, accumulation);
            }
            output << voice.position << ' ' << (voice.flags & 1);
            for (const auto value : accumulation) { output << ' ' << value; }
            for (const auto value : accumulation) { output << ' ' << swiv3d::detail::digital_sample(value); }
            output << '\n';
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
