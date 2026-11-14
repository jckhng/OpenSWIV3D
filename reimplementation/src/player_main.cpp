#include "input_script.hpp"
#include "checkpoint.hpp"
#ifdef _WIN32
#include "window.hpp"
#include "audio_output.hpp"
#include "clock.hpp"
#include "video_presenter.hpp"
#include "device_input.hpp"
#endif

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

enum class ReplayMode { flight, combat, campaign };
enum class CheckpointMode { segments, final_only };

using swiv3d::detail::checkpoint;

void replay(swiv3d::detail::MissionSession& session, const std::filesystem::path& movie,
            const std::filesystem::path& output, ReplayMode mode, CheckpointMode checkpoints) {
    if (std::filesystem::exists(output)) {
        throw std::runtime_error("Flight output directory must be fresh");
    }
    std::ifstream script(movie);
    const auto segments = swiv3d::detail::read_flight(script);
    std::filesystem::create_directories(output);
    auto frame = mode == ReplayMode::campaign ? swiv3d::detail::render_session(session) : session.mission->render(session.globals);
    checkpoint(*session.mission, frame, output / "initial");
    std::size_t ordinal = 0;
    for (const auto& segment : segments) {
        for (std::uint32_t i = 0; i < segment.updates; ++i) {
            if (mode == ReplayMode::campaign) { swiv3d::detail::advance_session(session, segment.keys); }
            else if (mode == ReplayMode::combat) { session.mission->advance(segment.keys); }
            else { session.mission->advance_flight(segment.keys); }
            frame = mode == ReplayMode::campaign ? swiv3d::detail::render_session(session) : session.mission->render(session.globals);
        }
        if (checkpoints == CheckpointMode::segments) {
            checkpoint(*session.mission, frame, output / ("step-" + std::to_string(ordinal)));
        }
        ++ordinal;
        std::cout << "Mission tick " << session.mission->state().tick << '\n';
    }
    if (checkpoints == CheckpointMode::final_only) {
        checkpoint(*session.mission, frame, output / "final");
    }
}

#ifdef _WIN32
void play(swiv3d::detail::MissionSession& session, const std::filesystem::path& output) {
    session.movie_timing = swiv3d::detail::MovieTiming::device;
    if (!output.empty()) {
        if (std::filesystem::exists(output)) {
            throw std::runtime_error("Live flight output directory must be fresh");
        }
        std::filesystem::create_directories(output);
    }
    swiv3d::host::Window window;
    window.escape_action(swiv3d::host::EscapeAction::forward);
    swiv3d::host::AudioOutput audio;
    swiv3d::detail::CompatibilityClock compatibility_clock(swiv3d::detail::device_audio_rate);
    swiv3d::host::DeviceInputHost input;
    using Clock = std::chrono::steady_clock;
    const auto interval = std::chrono::microseconds(1000000 / swiv3d::mission_tick_rate);
    auto next = Clock::now();
    auto previous = next;
    swiv3d::RenderTiming timing;
    timing.counter_reset = swiv3d::FrameCounterReset::event_loop_end;
    auto frame = swiv3d::detail::render_session(session, timing);
    swiv3d::detail::VideoPresenter video(frame,swiv3d::detail::RasterSchedule(
        {},{},swiv3d::detail::IrqScheduling::device,
        [&](std::uint64_t interrupt) { return session.audio->irq_work(interrupt); }));
    const auto startup_cycles = session.audio->device_cycles();
    constexpr auto cpu_frequency = swiv3d::detail::CompatibilityClock::cpu_frequency;
    const auto startup_ns = startup_cycles / cpu_frequency * UINT64_C(1000000000) +
        startup_cycles % cpu_frequency * UINT64_C(1000000000) / cpu_frequency;
    compatibility_clock.elapse(startup_ns);
    compatibility_clock.drain();
    compatibility_clock.drain_audio();
    video.elapse(startup_ns);
    std::uint64_t presented_retraces = 0;
    const auto advance_clocks = [&](Clock::time_point now) {
        const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous).count());
        compatibility_clock.elapse(elapsed);
        video.elapse(elapsed);
        previous = now;
        if (video.retraces() == presented_retraces) { return; }
        presented_retraces = video.retraces();
        const auto& visible = video.displayed();
        if (!visible.pixels.empty()) { window.present(visible); }
    };
    if (!output.empty() && session.mission) {
        checkpoint(*session.mission, frame, output / "initial");
    }
    window.present(frame);
    compatibility_clock.rendered();
    while (true) {
        if (!window.poll()) { break; }
        if (!window.focused()) {
            input.poll(swiv3d::host::InputFocus::background,swiv3d::host::PointerMode::free);
            next = previous = Clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        const auto now = Clock::now();
        advance_clocks(now);
        if (now < next) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const auto updates = compatibility_clock.drain();
        const bool mouse_control = session.frontend->state().phase == swiv3d::FrontendPhase::mission_request &&
            session.mission->state().flight.controller == swiv3d::ControllerKind::mouse;
        const auto devices = input.poll(swiv3d::host::InputFocus::foreground,mouse_control ? swiv3d::host::PointerMode::relative : swiv3d::host::PointerMode::free);
        session.frontend->devices({true,devices.joystick_connected});
        audio.submit(swiv3d::detail::mix_session_device(session, compatibility_clock.drain_audio()));
        for (std::uint32_t update = 0; update < updates; ++update) {
            swiv3d::detail::advance_session(session, window.keys(),devices);
            compatibility_clock.dispatched();
            if (session.frontend->state().phase == swiv3d::FrontendPhase::quit) { break; }
        }
        if (session.frontend->state().phase == swiv3d::FrontendPhase::quit) { break; }
        if (session.mission) { session.mission->finish_event_loop(); }
        next = now + interval;
        timing.frames_per_second = static_cast<int>(compatibility_clock.frame_rate());
        frame = swiv3d::detail::render_session(session, timing, window.keys());
        compatibility_clock.rendered();
        advance_clocks(Clock::now());
        video.begin_frame(frame);
    }
    if (!output.empty() && session.mission) {
        checkpoint(*session.mission, frame, output / "final");
    }
    swiv3d::detail::abort_mission(session);
}
#endif

}

int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            throw std::runtime_error("Usage: swiv3d_player GAME FRONTEND|--frontend [MOVIE OUTPUT [--combat|--campaign] [--final-only] | --record OUTPUT] [--profile FILE] [--cd-cue FILE]");
        }
        std::filesystem::path profile, cd_cue;
        auto checkpoints = CheckpointMode::segments;
        std::vector<std::string> arguments;
        for (int i = 3; i < argc; ++i) {
            if (std::string(argv[i]) == "--profile") {
                if (++i == argc || !profile.empty()) { throw std::runtime_error("Expected one profile path"); }
                profile = argv[i];
            } else if (std::string(argv[i]) == "--cd-cue") {
                if (++i == argc || !cd_cue.empty()) { throw std::runtime_error("Expected one CD cue path"); }
                cd_cue = argv[i];
            } else if (std::string(argv[i]) == "--final-only") {
                checkpoints = CheckpointMode::final_only;
            } else { arguments.emplace_back(argv[i]); }
        }
        if (checkpoints == CheckpointMode::final_only && (arguments.size() < 2 || arguments[0] == "--record")) {
            throw std::runtime_error("--final-only requires a replay movie and output directory");
        }
        const auto movies = arguments.empty() || arguments[0] == "--record" ?
            swiv3d::detail::MoviePlayback::play : swiv3d::detail::MoviePlayback::skip;
        const bool frontend = std::string(argv[2]) == "--frontend";
        if (frontend && movies == swiv3d::detail::MoviePlayback::skip) { throw std::runtime_error("Interactive frontend requires the window host"); }
        auto session = frontend ? swiv3d::detail::start_frontend(argv[1],profile) :
            swiv3d::detail::start_mission(argv[1],argv[2],profile,movies);
        if (!cd_cue.empty()) { session.audio->open_music(cd_cue); }
        if (arguments.size() >= 2 && arguments[0] != "--record") {
            ReplayMode mode = ReplayMode::flight;
            if (arguments.size() == 3 && arguments[2] == "--combat") { mode = ReplayMode::combat; }
            else if (arguments.size() == 3 && arguments[2] == "--campaign") { mode = ReplayMode::campaign; }
            else if (arguments.size() != 2) { throw std::runtime_error("Unknown replay option"); }
            replay(session, arguments[0], arguments[1], mode, checkpoints);
            return 0;
        }
        if (!arguments.empty() && (arguments.size() != 2 || arguments[0] != "--record")) { throw std::runtime_error("Unknown player option"); }
#ifdef _WIN32
        play(session, arguments.empty() ? std::filesystem::path{} : std::filesystem::path(arguments[1]));
#else
        throw std::runtime_error("Interactive window host currently requires Windows; flight replay is portable");
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
