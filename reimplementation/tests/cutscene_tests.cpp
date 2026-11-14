#include "../src/cutscene.hpp"
#include "../src/device_audio.hpp"
#include "../src/smacker_work.hpp"
#include "../src/smacker_loading.hpp"
#include "../src/smacker_stream.hpp"
#include "../src/smacker_clock.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {

void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

void save(const std::filesystem::path& path, const void* bytes, std::size_t size) {
    std::ofstream output(path, std::ios::binary);
    output.write(static_cast<const char*>(bytes), size);
    require(static_cast<bool>(output), "Cannot write cutscene fixture");
}

void put(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) { bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8)); }
}

void synthetic() {
    using swiv3d::detail::MoviePreparation;
    MoviePreparation preparation;
    preparation.cpu(100);
    preparation.disk(100);
    preparation.cpu(100);
    preparation.elapse(150,50);
    require(preparation.remaining() == 250,"CPU preparation bypassed IRQ service");
    preparation.elapse(100,100);
    require(preparation.remaining() == 150,"Disk wait did not start after the CPU stage");
    preparation.elapse(100,0);
    require(preparation.remaining() == 100,"Disk wait did not overlap IRQ service");
    preparation.elapse(100,100);
    require(preparation.remaining() == 0,"Preparation did not resume after disk completion");
    MoviePreparation eof;
    eof.cpu(1);
    eof.disk(0);
    eof.cpu(1);
    eof.elapse_device(2,2,0,[](auto) { return 0; });
    require(eof.remaining() != 0,"Zero-byte disk read omitted the idle callback");
    std::vector<std::uint8_t> strings(35);
    put(strings, 0, 0x630002);
    put(strings, 4, 16);
    put(strings, 8, UINT32_MAX);
    put(strings, 20, 32);
    put(strings, 24, UINT32_MAX);
    strings[32] = strings[33] = '0';
    std::vector<std::uint8_t> archive;
    std::vector<swiv3d::Resource> directory;
    const auto append = [&](unsigned id, const std::vector<std::uint8_t>& data) {
        directory.push_back({static_cast<unsigned>(archive.size()), static_cast<unsigned>(data.size()), id});
        archive.insert(archive.end(), data.begin(), data.end());
    };
    append(100, strings);
    append(507, std::vector<std::uint8_t>(4));
    append(508, std::vector<std::uint8_t>(384 * 4));
    append(800, std::vector<std::uint8_t>(65536));
    const auto table = archive.size();
    archive.resize(table + (directory.size() + 1) * 12);
    for (std::size_t i = 0; i < directory.size(); ++i) {
        put(archive, table + i * 12, directory[i].offset);
        put(archive, table + i * 12 + 4, directory[i].size);
        put(archive, table + i * 12 + 8, directory[i].id);
    }
    put(archive, archive.size() - 12, static_cast<unsigned>(directory.size() + 1));
    std::vector<std::uint8_t> movie(123);
    std::copy_n("SMK2", 4, movie.begin());
    put(movie, 4, swiv3d::image_width);
    put(movie, 8, swiv3d::image_height);
    put(movie, 12, 2);
    put(movie, 16, 100);
    put(movie, 52, 1);
    put(movie, 104, 4);
    put(movie, 108, 4);
    auto streamed = movie;
    constexpr unsigned streamed_frames = 8, streamed_frame_bytes = 4096;
    put(streamed,12,streamed_frames);
    streamed.resize(104 + streamed_frames * 5 + 1 + streamed_frames * streamed_frame_bytes);
    for (unsigned frame = 0; frame < streamed_frames; ++frame) { put(streamed,104 + frame * 4,streamed_frame_bytes); }
    swiv3d::detail::SmackerStream stream(streamed);
    require(stream.state().position == 8192 && stream.state().cached == 3951,
            "Streaming first frame lost its cached tail");
    for (unsigned frame = 1; frame < streamed_frames; ++frame) { stream.next(); }
    const auto exhausted = stream.state();
    for (unsigned poll = 0; poll < 2; ++poll) {
        const auto read = stream.background();
        require(std::count_if(read.stages().begin(),read.stages().end(),[](const auto& stage) {
                    return stage.kind == swiv3d::detail::PreparationKind::disk && stage.work == 0;
                }) == 1 && stream.state().position == exhausted.position && stream.state().cached == 0,
                "Streaming EOF suppressed subsequent background reads");
    }
    const std::vector<std::uint8_t> constant_pcm{0x0b,0x40,1,0,0,0};
    require(swiv3d::detail::smacker_work::StereoPcm(constant_pcm,0,constant_pcm.size(),4).work() == 173,
            "Original first-frame PCM decode work was omitted");
    require(swiv3d::detail::smacker_work::StereoPcm(constant_pcm,0,constant_pcm.size(),47232).work() == 425225,
            "Constant PCM tree incorrectly consumed sample bits");
    bool pcm_truncated = false;
    try { swiv3d::detail::smacker_work::StereoPcm(constant_pcm,0,1,4).work(); }
    catch (const std::runtime_error&) { pcm_truncated = true; }
    require(pcm_truncated,"Truncated PCM tree was accepted");
    const auto disk_requests = swiv3d::detail::smacker_loading::disk_requests(movie);
    require(disk_requests == std::vector<std::uint32_t>{1024,104,8,2,1,8}, "Small movie disk requests differ");
    require(swiv3d::detail::smacker_loading::disk_wait_work(disk_requests) != 0, "Movie disk latency was omitted");
    auto large_tables = movie;
    put(large_tables,52,65536);
    large_tables.resize(104 + 10 + 65536 + 4096);
    std::vector<std::uint32_t> split_reads{1024,104,8,2};
    split_reads.insert(split_reads.end(),7,8192);
    split_reads.insert(split_reads.end(),{4096,4096,3982});
    require(swiv3d::detail::smacker_loading::disk_requests(large_tables) == split_reads,
            "RAD/DOS transfer boundaries were merged");
    put(large_tables,12,UINT32_MAX);
    bool truncated = false;
    try { swiv3d::detail::smacker_loading::disk_requests(large_tables); }
    catch (const std::runtime_error&) { truncated = true; }
    require(truncated,"Truncated movie index was accepted");
    const auto path = std::filesystem::temp_directory_path() /
        ("swiv3d-cutscene-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
    const auto cleanup = [&] {
        std::filesystem::remove(path / "TEST.SMK");
        std::filesystem::remove(path / "PALETTE.SMK");
        std::filesystem::remove(path);
    };
    try {
        save(path / "TEST.SMK", movie.data(), movie.size());
        swiv3d::detail::Cutscene scene(path, "TEST", swiv3d::Archive(archive), {});
        require(scene.frames() == 2 && scene.frame() == 1 && scene.frame_duration() == 100000, "Wrong synthetic movie metadata");
        const auto first = scene.render();
        require(first.pixels.size() == swiv3d::image_width * swiv3d::image_height &&
                std::all_of(first.pixels.begin(), first.pixels.end(), [](auto pixel) { return pixel == 0; }), "Wrong synthetic frame");
        require(!scene.advance(99999) && scene.frame() == 1, "Movie frame hold ended early");
        require(!scene.advance(1) && scene.frame() == 2, "Movie deadline missed");
        require(scene.render().pixels == first.pixels && scene.frame() == 2, "Render mutated movie state");
        require(scene.advance(100000) && scene.advance(1), "Movie did not retain EOF");
        swiv3d::detail::Cutscene suspended(path,"TEST",swiv3d::Archive(archive),{});
        require(!suspended.advance_device(swiv3d::detail::device_audio_rate / 10,swiv3d::detail::MoviePolling::suspended) &&
                suspended.frame() == 1,"Video decoding polled the successor frame");
        require(!suspended.advance_device(0) && suspended.frame() == 2,"Suspended polling lost elapsed device time");
        const auto pcm = scene.mix(100);
        require(pcm.size() == 200 && std::all_of(pcm.begin(), pcm.end(), [](auto sample) { return sample == 0; }), "Silent movie produced audio");
        movie.resize(115);
        put(movie, 104, 16);
        put(movie, 108, 12);
        movie[112] = movie[113] = 1;
        movie.insert(movie.end(), {3,1,2,3,4,5,6,0xff,0xfd,0,0,0, 0,0,0,0,
                                  2,0x80,0x41,0,0xff,0xfc,0,0, 0,0,0,0});
        save(path / "PALETTE.SMK", movie.data(), movie.size());
        swiv3d::detail::Cutscene palette(path, "PALETTE", swiv3d::Archive(archive), {});
        require(!palette.advance(100000), "Palette fixture ended early");
        const auto colours = palette.render().palette;
        require(colours[3] == 1 && colours[4] == 2 && colours[5] == 3 &&
                colours[6] == 4 && colours[7] == 5 && colours[8] == 6, "Overlapping palette copy lost previous-frame colours");
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 1) { synthetic(); return 0; }
        if (argc == 3 && std::string(argv[1]) == "--clock-cases") {
            std::ifstream input(argv[2]);
            std::uint32_t duration, rate, frame, elapsed;
            while (input >> duration >> rate >> frame >> elapsed) {
                require(frame != 0,"Movie clock frame must be positive");
                swiv3d::detail::SmackerClock clock(duration,rate);
                for (unsigned index = 1; index < frame; ++index) { clock.next(); }
                clock.elapse(elapsed);
                std::cout << clock.ready() << '\n';
            }
            require(input.eof(),"Invalid movie clock cases");
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "--stream-steps") {
            std::ifstream file(std::filesystem::path(argv[2]) / "ANIMS" / (std::string(argv[3]) + ".SMK"),std::ios::binary);
            require(static_cast<bool>(file),"Cannot read movie stream fixture");
            const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
            swiv3d::detail::SmackerStream stream(bytes);
            std::ifstream input(argv[4]);
            const auto print = [&](const auto& plan) {
                const auto& state = stream.state();
                std::cout << state.position << ' ' << state.cached << ' ' << state.capacity << ' '
                          << state.read << ' ' << state.write << ' ' << state.end << ' '
                          << state.background_bytes << ' ' << state.high_water << ' ' << plan.remaining();
                for (const auto& stage : plan.stages()) {
                    std::cout << ' ' << (stage.kind == swiv3d::detail::PreparationKind::cpu ? 'c' : 'd') << stage.work;
                }
                std::cout << '\n';
            };
            print(swiv3d::detail::MoviePreparation{});
            std::string operation;
            while (input >> operation) {
                require(operation == "next" || operation == "background","Invalid streaming operation");
                print(operation == "next" ? stream.next() : stream.background());
            }
            require(input.eof(),"Cannot read streaming operations");
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--packet-work") {
            std::ifstream input(std::filesystem::path(argv[2]) / "ANIMS" / (std::string(argv[3]) + ".SMK"),std::ios::binary);
            require(static_cast<bool>(input),"Cannot read movie packet fixture");
            const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
            const auto frames = static_cast<std::uint32_t>(bytes.at(12)) | std::uint32_t(bytes.at(13)) << 8 |
                std::uint32_t(bytes.at(14)) << 16 | std::uint32_t(bytes.at(15)) << 24;
            for (unsigned frame = 0; frame < frames; ++frame) {
                std::cout << swiv3d::detail::smacker_work::pcm_work(bytes,frame) << '\n';
            }
            return 0;
        }
        if ((argc == 4 || argc == 5) && (std::string(argv[1]) == "--work" || std::string(argv[1]) == "--disk-work" || std::string(argv[1]) == "--pcm-work" || std::string(argv[1]) == "--video-work" || std::string(argv[1]) == "--caller-work" || std::string(argv[1]) == "--opening-stages")) {
            const auto file = std::filesystem::path(argv[2]) / "ANIMS" / (std::string(argv[3]) + ".SMK");
            std::ifstream input(file,std::ios::binary);
            require(static_cast<bool>(input),"Cannot read movie work fixture");
            const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
            if (std::string(argv[1]) == "--opening-stages") {
                require(argc == 4,"Opening stages take GAME and NAME");
                const auto reads = swiv3d::detail::smacker_loading::opening_reads(bytes);
                const auto stages = swiv3d::detail::smacker_loading::preparation(bytes,reads,0,0);
                for (const auto& stage : stages.stages()) {
                    std::cout << (stage.kind == swiv3d::detail::PreparationKind::cpu ? "cpu" : "disk") << ' ' << stage.work << '\n';
                }
                return 0;
            }
            if (std::string(argv[1]) == "--caller-work") {
                require(argc == 4,"Caller work takes GAME and NAME");
                const auto reads = swiv3d::detail::smacker_loading::opening_reads(bytes);
                std::cout << swiv3d::detail::smacker_loading::caller_work(bytes,reads) << ' ' << reads.bridge_work << '\n';
                return 0;
            }
            if (std::string(argv[1]) == "--video-work") {
                require(argc == 4,"Video work takes GAME and NAME");
                std::cout << swiv3d::detail::smacker_work::Tables(bytes).first_video_work() << '\n';
                return 0;
            }
            if (std::string(argv[1]) == "--pcm-work") {
                require(argc == 4,"PCM work takes GAME and NAME");
                std::cout << swiv3d::detail::smacker_work::first_pcm_work(bytes) << '\n';
                return 0;
            }
            if (std::string(argv[1]) == "--disk-work") {
                require(argc == 4,"Disk work takes GAME and NAME");
                const auto requests = swiv3d::detail::smacker_loading::disk_requests(bytes);
                std::cout << swiv3d::detail::smacker_loading::disk_wait_work(requests) << '\n';
                for (auto request : requests) { std::cout << request << '\n'; }
                return 0;
            }
            swiv3d::detail::smacker_work::CacheSlack slack{};
            if (argc == 5) {
                std::ifstream input(argv[4]);
                for (auto& tree : slack) {
                    for (auto& value : tree) { require(static_cast<bool>(input >> value),"Missing movie cache slack"); }
                }
            }
            std::cout << swiv3d::detail::smacker_work::Tables(bytes,slack).work() << '\n';
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--resources") {
            const std::filesystem::path game = argv[2];
            const auto globals = swiv3d::Archive::open(game / "GLOBAL.BD");
            for (const auto& file : std::filesystem::directory_iterator(game / "ANIMS")) {
                if (file.path().extension() != ".SMK") { continue; }
                const auto name = file.path().stem().string();
                swiv3d::FrontendState request;
                if (name.rfind("BR_TER", 0) == 0 && name.back() >= '0' && name.back() <= '6') {
                    request.level = static_cast<unsigned>(name.back() - '0');
                }
                swiv3d::detail::Cutscene scene(game / "ANIMS", name, globals, request);
                for (unsigned frame = 1; frame <= scene.frames(); ++frame) {
                    scene.render();
                    scene.mix(scene.frame_duration() * 44100ULL / 1000000);
                    require(scene.advance(scene.frame_duration()) == (frame == scene.frames()), "Resource EOF differs");
                }
                std::cout << name << ' ' << scene.frames() << '\n';
            }
            return 0;
        }
        require(argc == 4, "Expected GAME NAME OUTPUT");
        const std::filesystem::path game = argv[1], output = argv[3];
        require(!std::filesystem::exists(output), "Output must be fresh");
        std::filesystem::create_directories(output);
        swiv3d::FrontendState request;
        request.level = std::string(argv[2]) == "OUTRO" ? 16 : std::string(argv[2]) == "BR_TER0" ? 0 : 1;
        const auto globals = swiv3d::Archive::open(game / "GLOBAL.BD");
        swiv3d::detail::Cutscene cutscene(game / "ANIMS", argv[2], globals, request);
        require(cutscene.frame() == 1, "Playback must start on frame one");
        const auto initial = cutscene.render();
        require(cutscene.render().pixels == initial.pixels && cutscene.frame() == 1, "Rendering advanced the movie");
        require(!cutscene.advance(cutscene.frame_duration() - 1) && cutscene.frame() == 1, "Frame hold ended early");
        require(!cutscene.advance(1) && cutscene.frame() == 2, "Frame did not advance at its deadline");
        swiv3d::detail::Cutscene replay(game / "ANIMS", argv[2], globals, request);
        std::ofstream metadata(output / "frames.txt"), audio(output / "audio.pcm", std::ios::binary);
        metadata << replay.frames() << ' ' << replay.frame_duration() << '\n';
        const auto duration = static_cast<std::uint64_t>(replay.frames()) * replay.frame_duration();
        std::uint64_t samples = 0;
        for (std::uint32_t frame = 1; frame <= replay.frames(); ++frame) {
            const auto image = replay.render();
            save(output / (std::to_string(frame) + ".indexed"), image.pixels.data(), image.pixels.size());
            save(output / (std::to_string(frame) + ".palette"), image.palette.data(), image.palette.size());
            const auto last_sample = static_cast<std::uint64_t>(frame) * replay.frame_duration() * 44100 / 1000000;
            const auto pcm = replay.mix(static_cast<std::size_t>(last_sample - samples));
            audio.write(reinterpret_cast<const char*>(pcm.data()), pcm.size() * sizeof(std::int16_t));
            samples = last_sample;
            require(replay.advance(replay.frame_duration()) == (frame == replay.frames()), "Incorrect natural EOF");
        }
        require(samples == duration * 44100 / 1000000, "Audio length drifted");
        require(replay.advance(1), "Completed playback restarted");
        std::cout << "Cutscene frames " << replay.frames() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
