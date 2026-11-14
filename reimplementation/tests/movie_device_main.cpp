#include "../src/input_script.hpp"
#include "../src/clock.hpp"
#include "../src/smacker_work.hpp"
#include "../src/smacker_loading.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

namespace {
void trace_handoff(const char* game, std::uint64_t frames, const char* path) {
    using namespace swiv3d::detail;
    auto session = start_frontend(game);
    session.movie_timing = MovieTiming::device;
    const auto initialization_cycles = session.audio->device_cycles();
    std::ofstream output(path);
    std::string previous;
    for (std::uint64_t frame = 0; frame <= frames; ++frame) {
        const auto& sound = session.audio->movie_sound_state();
        const auto& stages = session.movie_preparation.stages();
        const auto stage = std::find_if(stages.begin(),stages.end(),[](const auto& value) { return value.work != 0; });
        std::ostringstream state;
        state << "\"opening\":" << session.opening_movie
              << ",\"frame\":" << (session.cutscene ? session.cutscene->frame() : 0)
              << ",\"preparing\":" << (session.movie_preparation_work != 0)
              << ",\"preparation_stage\":" << (stage - stages.begin())
              << ",\"playing\":" << sound.playing
              << ",\"available\":" << sound.available << ",\"queued\":" << sound.queued
              << ",\"completed\":" << sound.completed << ",\"epoch\":";
        if (sound.epoch) { state << *sound.epoch; } else { state << "null"; }
        state << ",\"current\":" << sound.current << ",\"last_ready\":" << sound.last_ready
              << ",\"lengths\":[" << sound.lengths[0] << ',' << sound.lengths[1]
              << "],\"positions\":[" << sound.positions[0] << ',' << sound.positions[1] << ']';
        if (state.str() != previous) {
            previous = state.str();
            output << "{\"output_frame\":" << frame << ",\"initialization_cycles\":" << initialization_cycles
                   << ",\"preparation_work\":" << session.movie_preparation_work
                   << ",\"clock_ms\":" << session.audio->movie_timestamp(0);
            if (frame == 0 && session.audio->opening_clock()) {
                const auto& clock = *session.audio->opening_clock();
                output << ",\"startup\":{\"wait_return\":" << clock.wait_return
                       << ",\"first_timestamp\":" << clock.first_timestamp
                       << ",\"driver_entry\":" << clock.driver_entry
                       << ",\"activation\":" << clock.activation << '}';
            }
            output << ',' << previous << "}\n";
        }
        if (frame != frames) { mix_session_device(session,1); }
    }
    if (!output) { throw std::runtime_error("Cannot write movie lifecycle trace"); }
}

void require(bool value,const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

void test_timing(const char* game) {
    using namespace swiv3d;
    auto timed = detail::start_frontend(game);
    require(timed.audio->movie_sound_state().epoch.value_or(0) != 0,
            "Opening movie discarded timer initialization before audio activation");
    timed.movie_timing = detail::MovieTiming::device;
    detail::mix_session_device(timed,detail::device_audio_rate * 71 / 1000);
    require(timed.cutscene->frame() == 1,"Packet decoding completed without consuming CPU work");
    detail::mix_session_device(timed,detail::device_audio_rate * 4 / 1000);
    require(timed.cutscene->frame() == 2,"Movie clock waited for a game update");
    for (unsigned tick = 0; tick < 100; ++tick) { detail::advance_session(timed,{}); }
    require(timed.cutscene->frame() == 2,"Game updates advanced the device-owned movie twice");
    auto bulk_handoff = detail::start_frontend(game);
    auto sample_handoff = detail::start_frontend(game);
    bulk_handoff.movie_timing = sample_handoff.movie_timing = detail::MovieTiming::device;
    constexpr std::size_t handoff_window = detail::device_audio_rate * 224 / 10;
    detail::mix_session_device(bulk_handoff,handoff_window);
    for (std::size_t frame = 0; frame < handoff_window; ++frame) { detail::mix_session_device(sample_handoff,1); }
    require(bulk_handoff.opening_movie == sample_handoff.opening_movie &&
            bulk_handoff.movie_preparation_work == sample_handoff.movie_preparation_work,
            "Output partition changed the handoff CPU budget");
    auto batched = detail::start_frontend(game);
    auto partitioned = detail::start_frontend(game);
    batched.movie_timing = partitioned.movie_timing = detail::MovieTiming::device;
    constexpr std::size_t movie_window = detail::device_audio_rate * 24;
    const auto batch_pcm = detail::mix_session_device(batched,movie_window);
    std::vector<std::int16_t> split_pcm;
    constexpr std::array<std::size_t,5> chunks{1,47,137,13,997};
    for (std::size_t position = 0, chunk = 0; position < movie_window; ++chunk) {
        const auto count = std::min(chunks[chunk % chunks.size()],movie_window - position);
        const auto samples = detail::mix_session_device(partitioned,count);
        split_pcm.insert(split_pcm.end(),samples.begin(),samples.end());
        position += count;
    }
    require(batch_pcm == split_pcm && batched.opening_movie == 2 && partitioned.opening_movie == 2 &&
            batched.cutscene->frame() == partitioned.cutscene->frame(),
            "Audio buffer partition changed autonomous movie playback");
    auto loading = detail::start_frontend(game);
    detail::complete_cutscene(loading,detail::CutsceneEnd::finished);
    std::ifstream movie_file(std::filesystem::path(game) / "ANIMS" / "INTRO2.SMK",std::ios::binary);
    const std::vector<std::uint8_t> movie_bytes{std::istreambuf_iterator<char>(movie_file),{}};
    constexpr unsigned original_pcm_work = 1425648;
    const auto table_work = std::make_unique<detail::smacker_work::Tables>(movie_bytes)->work();
    const auto opening_reads = detail::smacker_loading::opening_reads(movie_bytes);
    constexpr unsigned first_caller_before_audio = 355 + 4351;
    constexpr unsigned first_caller_after_audio = 283 + 4294;
    require(loading.movie_preparation_work == table_work + first_caller_before_audio +
            detail::smacker_loading::disk_wait_work(opening_reads.requests) + original_pcm_work +
            detail::smacker_loading::caller_work(movie_bytes,opening_reads) + opening_reads.bridge_work + 38468,
            "Movie loading omitted original first-frame PCM decoding");
    loading.movie_timing = detail::MovieTiming::device;
    const auto previous_picture = detail::render_session(loading).pixels;
    constexpr auto work_per_sample = detail::CompatibilityClock::cpu_frequency / detail::device_audio_rate;
    const auto preparation_samples = (loading.movie_preparation_work + work_per_sample - 1) / work_per_sample;
    detail::mix_session_device(loading,preparation_samples);
    while (loading.movie_preparation_work != 0) { detail::mix_session_device(loading,1); }
    require(!loading.movie_preparation_frame.pixels.empty() &&
            detail::render_session(loading).pixels == previous_picture,
            "First-frame video decoding exposed the successor picture early");
    require(loading.movie_video_work == 582481 + first_caller_after_audio,"First-frame video budget differs from the original");
    const auto video_samples = (loading.movie_video_work + work_per_sample - 1) / work_per_sample;
    detail::mix_session_device(loading,video_samples);
    while (loading.movie_video_work != 0) { detail::mix_session_device(loading,1); }
    require(loading.movie_video_work == 0 && loading.movie_preparation_frame.pixels.empty(),
            "First-frame video did not release its completed picture");
    std::cout << "Device movie timing, first-frame work and buffer partition passed\n";
}

void controlled_movie(swiv3d::detail::MissionSession& session, const char* events,
                      std::uint64_t frames, std::ofstream& output) {
    std::ifstream input(events);
    std::uint64_t release, restart;
    if (!(input >> release >> restart) || release >= restart || restart >= frames) {
        throw std::runtime_error("Invalid movie lifetime boundaries");
    }
    std::uint64_t elapsed = 0, fraction = 0;
    while (elapsed < frames) {
        if (elapsed == release) { session.cutscene.reset(); }
        if (elapsed == restart) {
            session.cutscene = std::make_unique<swiv3d::detail::Cutscene>(
                session.game / "ANIMS","INTRO2",session.globals,session.frontend->state());
            session.cutscene->attach_audio(*session.audio);
            fraction = 0;
        }
        auto count = std::min<std::uint64_t>(swiv3d::detail::device_audio_rate / 1000,frames - elapsed);
        for (auto boundary : {release,restart}) {
            if (boundary > elapsed) { count = std::min(count,boundary - elapsed); }
        }
        const auto samples = swiv3d::detail::mix_session_device(session,count);
        output.write(reinterpret_cast<const char*>(samples.data()),samples.size() * sizeof(std::int16_t));
        if (session.cutscene) {
            fraction += count * 1000000;
            session.cutscene->advance(static_cast<std::uint32_t>(fraction / swiv3d::detail::device_audio_rate));
            fraction %= swiv3d::detail::device_audio_rate;
        }
        elapsed += count;
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 5 && std::string(argv[1]) == "--trace") {
            trace_handoff(argv[2],std::stoull(argv[3]),argv[4]);
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--timing-test") { test_timing(argv[2]); return 0; }
        if (argc != 4 && argc != 5 && argc != 6) { throw std::runtime_error("Expected GAME FRAMES OUTPUT [--skip-startup-frame | --lifetime EVENTS]"); }
        auto session = swiv3d::detail::start_frontend(argv[1]);
        if (argc == 6) {
            if (std::string(argv[4]) != "--lifetime") { throw std::runtime_error("Unknown movie fixture control"); }
            std::ofstream output(argv[3],std::ios::binary);
            controlled_movie(session,argv[5],std::stoull(argv[2]),output);
            if (!output) { throw std::runtime_error("Cannot write controlled movie output"); }
            std::cout << "Controlled lifetime, native streamed decoding and device mixing\n";
            return 0;
        }
        session.movie_timing = swiv3d::detail::MovieTiming::device;
        if (argc == 5) {
            if (std::string(argv[4]) != "--skip-startup-frame") { throw std::runtime_error("Unknown movie fixture control"); }
            swiv3d::detail::mix_session_device(session,1);
        }
        std::ofstream output(argv[3],std::ios::binary);
        auto remaining = std::stoull(argv[2]);
        swiv3d::detail::CompatibilityClock clock(swiv3d::detail::device_audio_rate);
        constexpr auto frames_per_millisecond = swiv3d::detail::device_audio_rate / 1000;
        while (remaining != 0) {
            const auto frames = std::min<unsigned long long>(remaining,frames_per_millisecond);
            clock.elapse(frames * 1000000000 / swiv3d::detail::device_audio_rate);
            const auto samples = swiv3d::detail::mix_session_device(session,frames);
            output.write(reinterpret_cast<const char*>(samples.data()),samples.size() * sizeof(std::int16_t));
            const auto updates = clock.drain();
            for (unsigned update = 0; update < updates; ++update) { swiv3d::detail::advance_session(session,{}); }
            remaining -= frames;
        }
        if (!output) { throw std::runtime_error("Cannot write movie device output"); }
        if (!session.cutscene) { throw std::runtime_error("Movie device window crossed the retained movie owner"); }
        std::cout << "Retained movie " << session.opening_movie << " frame " << session.cutscene->frame() << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
