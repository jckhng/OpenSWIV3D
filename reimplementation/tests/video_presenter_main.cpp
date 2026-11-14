#include "../src/video_presenter.hpp"

#include <fstream>
#include <iostream>

namespace {
swiv3d::detail::RasterPhase read_phase(std::istream& events) {
    swiv3d::detail::RasterPhase phase;
    events >> phase.period_ms >> phase.frame_target_ms >> phase.line;
    for (auto& event : phase.events) { events >> event; }
    if (!events) { throw std::runtime_error("Missing raster event phase"); }
    return phase;
}

void schedule(const char* path, const char* service_path = nullptr) {
    std::ifstream events(path);
    const auto phase = read_phase(events);
    std::vector<swiv3d::detail::CpuServiceWindow> services;
    if (service_path != nullptr) {
        std::ifstream input(service_path);
        swiv3d::detail::CpuServiceWindow service{};
        while (input >> service.begin >> service.end) { services.push_back(service); }
        if (!input.eof()) { throw std::runtime_error("Invalid CPU service input"); }
    }
    swiv3d::detail::RasterSchedule raster(phase,std::move(services));
    std::uint64_t begin;
    unsigned page;
    while (events >> begin >> page) {
        const auto copy = raster.copy(begin,page);
        for (unsigned plane = 0; plane < 4; ++plane) {
            std::cout << "io " << copy.port_cycles[plane] << ' ' << ((1u << (plane + 8)) | 2) << '\n';
        }
        std::cout << "io " << copy.port_cycles[4] << ' ' << ((page << 15) | 12) << '\n';
        for (const auto& transfer : copy.transfers) {
            std::cout << "rep " << transfer.cycle - transfer.dwords << ' ' << transfer.remaining << '\n';
        }
    }
    if (!events.eof()) { throw std::runtime_error("Invalid raster copy event"); }
}

void idle_schedule(const char* path) {
    std::ifstream events(path);
    swiv3d::detail::RasterSchedule raster(read_phase(events));
    std::uint64_t begin, minimum;
    while (events >> begin >> minimum) { std::cout << raster.idle(begin,minimum) << '\n'; }
    if (!events.eof()) { throw std::runtime_error("Invalid disk idle event"); }
}

void idle_cases(const char* path) {
    std::ifstream input(path);
    if (!input) { throw std::runtime_error("Cannot read disk idle cases"); }
    while (input >> std::ws && input.peek() != std::char_traits<char>::eof()) {
        swiv3d::detail::RasterSchedule raster(read_phase(input));
        std::uint64_t begin, minimum;
        if (!(input >> begin >> minimum)) { throw std::runtime_error("Invalid disk idle case"); }
        std::cout << raster.idle(begin,minimum) << '\n';
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--idle-schedule") { idle_schedule(argv[2]); return 0; }
        if (argc == 3 && std::string(argv[1]) == "--idle-cases") { idle_cases(argv[2]); return 0; }
        if (argc == 3 && std::string(argv[1]) == "--timer-work") {
            std::ifstream input(argv[2]);
            unsigned message, bios;
            while (input >> message >> bios) {
                std::cout << swiv3d::detail::TimerService(message,bios).interrupt() << '\n';
            }
            if (!input.eof()) { throw std::runtime_error("Invalid timer work phases"); }
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--schedule") { schedule(argv[2]); return 0; }
        if (argc == 5 && std::string(argv[1]) == "--schedule" && std::string(argv[3]) == "--interrupts") {
            schedule(argv[2],argv[4]); return 0;
        }
        if (argc != 2) { throw std::runtime_error("Expected timed copy events"); }
        std::ifstream events(argv[1]);
        swiv3d::detail::VideoPhase phase;
        events >> phase.nanoseconds >> phase.displayed_page >> phase.requested_page >> phase.next_page;
        if (!events) { throw std::runtime_error("Missing initial VGA phase"); }
        swiv3d::MissionFrame frame;
        frame.pixels.resize(swiv3d::image_width * swiv3d::image_height);
        swiv3d::detail::VideoPresenter video(frame,phase);
        std::uint64_t time, previous = 0;
        unsigned begin;
        while (events >> time >> begin) {
            if (time < previous) { throw std::runtime_error("Unordered copy event"); }
            video.elapse(time-previous);
            previous = time;
            if (begin != 0) { video.begin_frame(frame); }
            std::cout << video.display_page() << ' ' << video.transferred_pixels() << ' ' << video.copying() << '\n';
        }
        if (!events.eof()) { throw std::runtime_error("Invalid copy event"); }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
