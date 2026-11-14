#include "../src/device_audio.hpp"

#include <fstream>
#include <iostream>

namespace {
swiv3d::detail::DeviceChannel channel(std::istream& input) {
    using namespace swiv3d::detail;
    unsigned rate, filter, loaded;
    std::size_t pending;
    DeviceChannelState state;
    input >> rate >> filter >> state.phase >> loaded;
    state.loaded = loaded != 0;
    for (auto& value : state.previous) { input >> value; }
    for (auto& value : state.current) { input >> value; }
    for (auto& value : state.history) { input >> value; }
    input >> pending;
    state.pending.resize(pending);
    for (auto& sample : state.pending) {
        for (auto& value : sample) { input >> value; }
    }
    if (!input || filter > 1) { throw std::runtime_error("Invalid device channel state"); }
    return DeviceChannel(rate,filter == 1 ? DeviceFilter::sound_blaster : DeviceFilter::none,std::move(state));
}

swiv3d::detail::StereoSample read(std::istream& input) {
    swiv3d::detail::StereoSample result{};
    input.read(reinterpret_cast<char*>(result.data()),sizeof(result));
    if (!input) { throw std::runtime_error("Captured device source exhausted"); }
    return result;
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 6 && std::string(argv[1]) == "--movie-batches") {
            std::ifstream state(argv[2]), operations(argv[3]), source(argv[4],std::ios::binary);
            std::ofstream output(argv[5],std::ios::binary);
            auto effects = channel(state);
            char operation;
            std::size_t frames;
            while (operations >> operation >> frames) {
                if (operation == 'P') { effects.prefetch(frames,[&] { return read(source); }); continue; }
                if (operation != 'M') { throw std::runtime_error("Unknown movie mixer operation"); }
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    const auto sample = swiv3d::detail::device_sum(effects.silence(),{});
                    output.write(reinterpret_cast<const char*>(sample.data()),sizeof(sample));
                }
            }
            if (!operations.eof() || !output) { throw std::runtime_error("Invalid movie mixer fixture"); }
            return 0;
        }
        if (argc != 6) { throw std::runtime_error("Expected STATE SB CD FRAMES OUTPUT"); }
        std::ifstream state(argv[1]);
        auto effects = channel(state), music = channel(state);
        std::ifstream sb(argv[2],std::ios::binary), cd(argv[3],std::ios::binary);
        std::ofstream output(argv[5],std::ios::binary);
        if (!sb || !cd || !output) { throw std::runtime_error("Cannot open device fixture files"); }
        const auto frames = std::stoull(argv[4]);
        for (std::uint64_t frame = 0; frame < frames; ++frame) {
            const auto sample = swiv3d::detail::device_sum(effects.next([&] { return read(sb); }),music.next([&] { return read(cd); }));
            output.write(reinterpret_cast<const char*>(sample.data()),sizeof(sample));
        }
        if (!output) { throw std::runtime_error("Cannot write device output"); }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
