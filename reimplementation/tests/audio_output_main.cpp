#include "../src/audio_output.hpp"
#include "../src/audio.hpp"

#include <chrono>
#include <iostream>
#include <thread>

int main() {
    try {
        swiv3d::host::AudioOutput output;
        const std::vector<std::int16_t> silence(swiv3d::detail::device_audio_rate / 40 * 2);
        for (int buffer = 0; buffer < 18; ++buffer) { output.submit(silence); }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::cout << "Native PCM device opened and recycled queued buffers\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
