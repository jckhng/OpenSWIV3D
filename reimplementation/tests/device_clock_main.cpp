#include "../src/video_clock.hpp"
#include "../src/sb_dma.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--timer-milliseconds") {
            std::ifstream events(argv[2]);
            swiv3d::detail::SoundBlasterDma timer;
            std::uint64_t current = 0, frames;
            const auto produce = [](auto&) { return false; };
            while (events >> frames) {
                if (frames < current) { throw std::runtime_error("Timer frames are out of order"); }
                while (current < frames) { timer.elapse_frame(produce); ++current; }
                std::cout << timer.timer_milliseconds() << '\n';
            }
            if (!events.eof()) { throw std::runtime_error("Invalid timer fixture"); }
            return 0;
        }
        if (argc != 2) { throw std::runtime_error("Expected device event file"); }
        std::ifstream events(argv[1]);
        std::uint64_t phase, previous = 0, time;
        unsigned visible, requested;
        if (!(events >> phase >> visible >> requested)) { throw std::runtime_error("Missing initial VGA phase/pages"); }
        swiv3d::detail::VideoClock clock;
        clock.request_page(visible);
        // Seed one complete frame and the observed phase, then retain the
        // pending CRTC address independently of the currently scanned page.
        clock.elapse((clock.dots_per_frame * 1000000000 + clock.dot_frequency - 1) / clock.dot_frequency + phase);
        clock.request_page(requested);
        int page;
        while (events >> time >> page) {
            if (time < previous) { throw std::runtime_error("Device timestamps are out of order"); }
            clock.elapse(time - previous);
            previous = time;
            std::cout << clock.display_page() << '\n';
            if (page >= 0) { clock.request_page(static_cast<unsigned>(page)); }
        }
        if (!events.eof()) { throw std::runtime_error("Invalid device event"); }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
