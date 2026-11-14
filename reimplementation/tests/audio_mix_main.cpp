#include "../src/audio.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

int main(int argc,char** argv) {
    try {
        if (argc < 3) { throw std::runtime_error("Expected external sound bank and fresh output directory [--gain-sweep] [--cd-cue FILE]"); }
        const std::filesystem::path output = argv[2];
        if (std::filesystem::exists(output)) { throw std::runtime_error("Audio mix output must be fresh"); }
        std::filesystem::create_directories(output);
        const auto bank = swiv3d::Archive::open(argv[1]);
        swiv3d::detail::Audio audio(bank);
        bool gain_sweep = false;
        for (int index = 3; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--gain-sweep") { gain_sweep = true; }
            else if (option == "--cd-cue" && index+1 < argc) { audio.open_music(argv[++index]); audio.music_track(3); }
            else if (option == "--movie-wave" && index+1 < argc) {
                std::ifstream wave(argv[++index],std::ios::binary);
                if (!wave) { throw std::runtime_error("Cannot open movie PCM fixture"); }
                audio.begin_movie(swiv3d::detail::decode_wave({std::istreambuf_iterator<char>(wave),{}}));
            }
            else { throw std::runtime_error("Unknown audio mix fixture option"); }
        }
        swiv3d::detail::SoundListener listener{{0,0,0},{256,0,0},{0,0,256}};
        int stage = 0;
        const auto source = [&](int actor) {
            return swiv3d::detail::SoundSource{{(actor-106)*90,0,60+stage*20},8000+(actor%3)*4000+stage*53};
        };
        audio.present({},listener,source);
        std::ofstream states(output / "voices.txt");
        std::ofstream pcm(output / "mixed.pcm",std::ios::binary);
        const auto save = [&] {
            for (const auto& voice : audio.voices()) {
                states << voice.sound << ' ' << voice.actor << ' ' << voice.flags << ' ' << voice.volume << ' '
                       << voice.pan << ' ' << voice.rate << ' ' << voice.position << '\n';
            }
        };
        for (; stage < 6; ++stage) {
            constexpr int gains[]{255,128,1,254,64,0};
            audio.effects_volume(gain_sweep ? gains[stage] : 255);
            std::vector<swiv3d::SoundEvent> events;
            if (stage == 0) {
                for (std::size_t i = 0; i < swiv3d::detail::sound_channels; ++i) {
                    events.push_back({0,static_cast<int>(100+i),static_cast<int>(bank.resources().at(i).id/100),swiv3d::SoundAction::loop});
                }
            }
            if (stage == 2) {
                for (int i : {4,8}) { events.push_back({0,100+i,static_cast<int>(bank.resources().at(i).id/100),swiv3d::SoundAction::stop}); }
            }
            if (stage == 3) { events.push_back({0,120,static_cast<int>(bank.resources().back().id/100),swiv3d::SoundAction::effect}); }
            if (stage == 5) { events.push_back({0,swiv3d::no_entity,0,swiv3d::SoundAction::stop_all}); }
            audio.present(events,listener,source);
            save();
            const auto mixed = audio.mix(4096);
            pcm.write(reinterpret_cast<const char*>(mixed.data()),mixed.size()*sizeof(std::int16_t));
            save();
        }
        std::cout << "Six heterogeneous 13-channel DMA blocks emitted\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
