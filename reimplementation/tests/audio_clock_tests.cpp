#include "../src/audio.hpp"
#include "../src/clock.hpp"
#include "../src/video_clock.hpp"
#include "../src/video_presenter.hpp"
#include "../src/sb_startup.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void movie_sound_fixture(const char* path, const char* pcm_path) {
    using namespace swiv3d::detail;
    std::ifstream input(path);
    std::ofstream pcm(pcm_path,std::ios::binary);
    MovieSound sound;
    unsigned rate = 0, channels = 0, bits = 0, duration = 0;
    const auto samples = [&](unsigned bytes, unsigned seed) {
        std::vector<std::int16_t> result;
        for (unsigned offset = 0; offset < bytes; offset += bits / 8) {
            const auto value = (offset / (bits / 8) + seed) % 256;
            result.push_back(static_cast<std::int16_t>((static_cast<int>(value) - 128) * 256));
        }
        return result;
    };
    char operation;
    unsigned value;
    while (input >> operation >> value) {
        std::int64_t played = -1;
        if (operation == 'O') {
            unsigned bytes, seed;
            rate = value;
            input >> channels >> bits >> duration >> bytes >> seed;
            if (bits != 8 && bits != 16) { throw std::runtime_error("Invalid movie fixture sample depth"); }
            sound.open({rate,static_cast<std::uint16_t>(channels),samples(bytes,seed),bits},duration);
        } else if (operation == 'A') {
            unsigned seed;
            input >> seed;
            sound.append(samples(value,seed));
        } else if (operation == 'S') { sound.service(value); }
        else if (operation == 'P') { sound.service(value); played = sound.played(value); }
        else if (operation == 'M') {
            std::vector<std::int32_t> buffer(value * 2);
            sound.merge(buffer,127);
            pcm.write(reinterpret_cast<const char*>(buffer.data()),buffer.size() * sizeof(std::int32_t));
        } else { throw std::runtime_error("Unknown movie sound operation"); }
        const auto& state = sound.state();
        std::cout << state.available << ' ' << state.queued << ' ' << state.completed << ' '
                  << (state.epoch ? std::int64_t(*state.epoch) : -1) << ' ' << state.current << ' '
                  << state.last_ready << ' ' << state.playing << ' ' << state.ended << ' '
                  << state.lengths[0] << ' ' << state.lengths[1] << ' '
                  << state.positions[0] << ' ' << state.positions[1] << ' ' << played << ' ' << state.decoded << '\n';
    }
    if (!input.eof() || !pcm) { throw std::runtime_error("Invalid movie sound fixture"); }
}

void require(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

void test_startup_scheduling() {
    using namespace swiv3d::detail;
    DspBusyClock dsp;
    for (unsigned slot = 0; slot < 8; ++slot) {
        require(dsp.busy(slot * 20),"DSP busy polling lost its initial bus slots");
    }
    require(!dsp.busy(160),"DSP busy polling never reached a ready slot");
    dsp.write(161);
    require(dsp.busy(180) && dsp.busy(222),"DSP busy flag cleared before its queued completion ran");
    dsp.complete();
    require(!dsp.busy(223),"DSP completion retained its command busy flag");
    require(!dsp.busy(2000),"DSP status did not resynchronize after an idle interval");
    RasterPhase phase;
    phase.events.fill(std::numeric_limits<double>::infinity());
    RasterSchedule plain(phase);
    require(plain.cpu(0,1000) == 1000,"Uninterrupted scalar work changed its duration");
    RasterSchedule atomic(phase);
    bool complete = false, observed = false;
    atomic.io_completion(60,[&] { complete = true; });
    const auto read_end = atomic.instruction(0,62,[&](auto) { observed = complete; });
    atomic.cpu(read_end,1);
    require(!observed && complete,"Device completion ran inside an atomic port instruction");
    RasterSchedule write(phase);
    require(write.instruction(0,46,[&](auto cycle) { write.io_completion(cycle + 60); }) == 47,
            "Queueing an earlier device event omitted the normal-core exit");
    phase.dac_ms = phase.dac_period_ms = .001;
    RasterSchedule whole(phase), split(phase), stopped(phase);
    const auto completed_work = whole.cpu(0,1000);
    const auto first = split.cpu(0,500);
    require(completed_work > 1000 && split.cpu(first,500) == completed_work,
            "Masked DAC budget exits changed with CPU work partitioning");
    stopped.stop_dac_clock();
    require(stopped.cpu(0,1000) == 1001,"Unmasking discarded the pending DAC event");
    SoundBlasterDma frames, cycles;
    const auto produce = [](auto&) { return false; };
    unsigned frame_irqs = 0, cycle_irqs = 0;
    constexpr auto half_frame = CompatibilityClock::cpu_frequency / device_audio_rate / 2;
    for (unsigned frame = 0; frame < 1000; ++frame) {
        if (frames.elapse_frame(produce) != SoundBlasterDma::Service::none) { ++frame_irqs; }
        cycles.elapse_cycles(half_frame,produce,[&](auto) { ++cycle_irqs; });
        cycles.elapse_cycles(half_frame,produce,[&](auto) { ++cycle_irqs; });
        require(cycles.timer_milliseconds() == frames.timer_milliseconds(),
                "Subsample initialization lost timer phase");
    }
    require(frame_irqs == cycle_irqs,"Subsample initialization changed IRQ delivery count");
}

void put(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) { bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8)); }
}

std::vector<std::uint8_t> wave() {
    std::vector<std::uint8_t> bytes(48);
    const auto tag = [&](std::size_t offset, const char* text) { std::copy_n(text, 4, bytes.begin() + offset); };
    tag(0, "RIFF"); put(bytes, 4, 40); tag(8, "WAVE"); tag(12, "fmt "); put(bytes, 16, 16);
    bytes[20] = 1; bytes[22] = 1; put(bytes, 24, swiv3d::detail::audio_rate); put(bytes, 28, swiv3d::detail::audio_rate);
    bytes[32] = 1; bytes[34] = 8; tag(36, "data"); put(bytes, 40, 4);
    bytes[44] = 0; bytes[45] = 128; bytes[46] = 255; bytes[47] = 128;
    return bytes;
}

swiv3d::Archive bank() {
    auto bytes = wave();
    const auto offset = bytes.size();
    bytes.resize(offset + 24);
    put(bytes, offset + 4, static_cast<unsigned>(offset)); put(bytes, offset + 8, 100004);
    put(bytes, offset + 12, 2);
    return swiv3d::Archive(std::move(bytes));
}

void test_decode_and_lifetime() {
    auto bytes = wave();
    const auto sample = swiv3d::detail::decode_wave(bytes);
    require(sample.rate == 44100 && sample.channels == 1 &&
            sample.samples == std::vector<std::int16_t>{-32768,0,32512,0}, "PCM decoder changed signed samples or rate");
    bytes.pop_back();
    bool rejected = false;
    try { swiv3d::detail::decode_wave(bytes); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "Truncated sound accepted");
    swiv3d::detail::Audio audio(bank());
    audio.present({{0,swiv3d::no_entity,1000}}, {}, [](int) { return swiv3d::detail::SoundSource{}; });
    const auto pcm = audio.mix(8);
    require(pcm[0] == -25856 && pcm[1] == -25856 && pcm[6] == -25856 && pcm[7] == -25856 && pcm[8] == 0 && pcm[9] == 0,
            "Sample amplitude table or one-shot expiry differs");
    require((audio.voices()[0].flags & 1) == 0, "One-shot did not finish");
    swiv3d::detail::Audio queued(bank());
    queued.enqueue({{0,swiv3d::no_entity,1000}});
    require(queued.mix(8) == pcm,"Queued interface audio differed from the existing effect mixer");
    swiv3d::detail::Audio muted(bank());
    muted.present({{0,swiv3d::no_entity,1000}}, {}, [](int) { return swiv3d::detail::SoundSource{}; });
    muted.effects_volume(0);
    const auto silence = muted.mix(8);
    require(std::all_of(silence.begin(),silence.end(),[](auto sample) { return sample == 0; }),
            "Effects volume had no live mixer consumer");
}

void test_digital_merge_contract() {
    const swiv3d::detail::PcmSample sample{11025,1,{1024,768,1280,1536,1792,2304,2048,1792},8};
    swiv3d::detail::SoundVoice voice{0,swiv3d::no_entity,1,1,81,11025,0};
    std::vector<std::int32_t> values(16);
    swiv3d::detail::merge_pcm(sample, voice, values);
    require(values == std::vector<std::int32_t>{11,16,8,12,14,20,17,24,20,28,25,36,23,32,20,28},
            "Original 8-bit amplitude-table contract differs");
    require(swiv3d::detail::digital_sample(-1) == -256 && swiv3d::detail::digital_sample(255) == 0 &&
            swiv3d::detail::digital_sample(50000) == 32512 && swiv3d::detail::digital_sample(-50000) == -32768,
            "Digital DMA conversion lost truncation or clipping");
    swiv3d::detail::Audio whole(bank()), split(bank());
    const std::vector<swiv3d::SoundEvent> events{{0,swiv3d::no_entity,1000,swiv3d::SoundAction::loop}};
    const auto source = [](int) { return swiv3d::detail::SoundSource{}; };
    whole.present(events,{},source); split.present(events,{},source);
    const auto expected = whole.mix(5000);
    std::vector<std::int16_t> actual;
    for (const auto frames : {1,3,1025,2000,1971}) {
        const auto part = split.mix(frames);
        actual.insert(actual.end(),part.begin(),part.end());
    }
    require(expected == actual, "Host buffer sizes changed digital mixer phase");
}

void test_master_gain_before_clipping() {
    for (const int volume : {0,1,2,64,127,128,254,255}) {
        swiv3d::detail::Audio audio(bank());
        audio.present({{0,swiv3d::no_entity,1000,swiv3d::SoundAction::loop},
                       {0,swiv3d::no_entity,1000,swiv3d::SoundAction::loop}}, {},
                      [](int) { return swiv3d::detail::SoundSource{}; });
        audio.effects_volume(volume);
        const int effective = 100 * (volume >> 1) / 127;
        const int expected = effective == 0 ? 0 : std::max(-32768,-(effective+1)*512);
        require(audio.mix(1)[0] == expected,"Master gain was applied after voice summation/clipping");
    }
    swiv3d::detail::Audio changed(bank()), retained(bank());
    const std::vector<swiv3d::SoundEvent> loop{{0,swiv3d::no_entity,1000,swiv3d::SoundAction::loop}};
    const auto source = [](int) { return swiv3d::detail::SoundSource{}; };
    changed.present(loop,{},source); retained.present(loop,{},source);
    changed.mix(1); retained.mix(1);
    changed.effects_volume(0);
    require(changed.mix(4095) == retained.mix(4095),"Gain change rewrote the pending DMA buffer");
    require(changed.mix(1) == std::vector<std::int16_t>(2),"New DMA buffer ignored the changed master gain");
}

void test_channel_order_and_spatial_delay() {
    swiv3d::detail::Audio audio(bank());
    std::vector<swiv3d::SoundEvent> events(14, {0,swiv3d::no_entity,1000});
    audio.present(events, {}, [](int) { return swiv3d::detail::SoundSource{}; });
    require(std::count_if(audio.voices().begin(), audio.voices().end(), [](const auto& voice) { return (voice.flags & 1) != 0; }) == 13,
            "Pending channel allocation did not preserve the 13-voice limit");
    audio.stop();
    swiv3d::detail::SoundListener listener{{{0,0,0}},{{256,0,0}},{{0,0,256}}};
    const auto source = [](int) { return swiv3d::detail::SoundSource{{{0,0,100}}, 22050}; };
    audio.present({{0,3000,1000,swiv3d::SoundAction::loop}}, listener, source);
    require(audio.voices()[0].volume == 127, "First sound render imported a future camera");
    audio.present({}, listener, source);
    require(audio.voices()[0].volume == 71 && audio.voices()[0].pan == 64 && audio.voices()[0].rate == 22050,
            "Spatial attenuation, pan or loop pitch differs");
    audio.mix(20);
    require((audio.voices()[0].flags & 1) != 0, "Loop ended at sample boundary");
    audio.present({{1,3000,1000,swiv3d::SoundAction::stop}}, listener, source);
    require((audio.voices()[0].flags & 1) == 0, "Actor-specific stop missed the loop");
}

void test_pit_and_frame_counters() {
    swiv3d::detail::CompatibilityClock whole, split;
    whole.elapse(1000000000);
    for (int i = 0; i < 1000; ++i) { split.elapse(1000000); }
    require(whole.drain() == 40 && split.drain() == 40, "PIT fractional phase changed with host polling cadence");
    require(whole.drain_audio() == 44100 && split.drain_audio() == 44100,
            "Audio sample clock drifted with PIT divisor or host polling cadence");
    whole.elapse(20000);
    require(whole.drain() == 0, "PIT callback occurred before the next measured period");
    whole.elapse(UINT64_C(10000000000));
    require(whole.drain() == 160, "Message queue overflow did not use the original 160-slot limit");
    require(whole.drain_audio() == 441000, "Dropping queued game messages also dropped elapsed audio time");
    for (int i = 0; i < 120; ++i) {
        if (i % 3 == 0) { split.rendered(); }
        split.dispatched();
    }
    require(split.frame_rate() == 13, "Three-second frame-rate window differs");
}

void test_cd_track_boundaries() {
    const auto cue = std::filesystem::current_path() / "swiv3d-audio-test.cue";
    const auto image = cue.parent_path() / "swiv3d-audio-test.bin";
    require(!std::filesystem::exists(cue) && !std::filesystem::exists(image), "Synthetic CD output already exists");
    {
        std::vector<std::uint8_t> bytes(2352 * 2);
        for (std::size_t offset = 2352; offset < bytes.size(); offset += 4) {
            bytes[offset] = 0xe8; bytes[offset + 1] = 3;
            bytes[offset + 2] = 0x18; bytes[offset + 3] = 0xfc;
        }
        std::ofstream output(image, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::ofstream description(cue);
        description << "FILE \"swiv3d-audio-test.bin\" BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 01 00:00:01\n";
    }
    {
        swiv3d::detail::CdAudio cd(cue);
        cd.play(2);
        std::vector<std::int16_t> pcm(600 * 2);
        cd.mix_into(pcm);
        require(pcm[0] == 1000 && pcm[1] == -1000 && pcm[1174] == 1000 && pcm[1176] == 0,
                "CD byte order, stereo channels or sector boundary differs");
        pcm.assign(4, 0); cd.mix_into(pcm);
        require(pcm == std::vector<std::int16_t>(4), "CD restarted before the original poll");
        cd.poll(); cd.mix_into(pcm);
        require(pcm[0] == 1000 && pcm[1] == -1000, "CD poll did not restart the selected track");
    }
    std::filesystem::remove(cue); std::filesystem::remove(image);
}

void test_video_retrace_latch() {
    swiv3d::detail::VideoClock whole, split;
    whole.request_page(1); split.request_page(1);
    whole.elapse(13092353);
    require(whole.display_page() == 0 && whole.retraces() == 0,"Page request bypassed VGA retrace latch");
    whole.elapse(1);
    require(whole.display_page() == 1 && whole.retraces() == 1,"VGA latch missed its dot-clock boundary");
    whole.request_page(0);
    whole.elapse(1000000000-13092354);
    for (int i = 0; i < 1000; ++i) { split.elapse(1000000); }
    require(whole.retraces() == 70 && split.retraces() == 70 && whole.display_page() == 0 && split.display_page() == 1,
            "VGA dot-clock phase changed with host polling or PIT callback cadence");
}

void test_observed_pit_terminal_count() {
    swiv3d::detail::CompatibilityClock clock;
    clock.elapse(24996186);
    require(clock.drain() == 0,"PIT game callback preceded the measured terminal count");
    clock.elapse(1);
    require(clock.drain() == 1,"PIT mode-2 period used the programmed divisor instead of terminal count one");
}

void test_miles_timer() {
    swiv3d::detail::SoundBlasterDma timer;
    const auto produce = [](auto&) { return false; };
    // Original AIL_ms_count with the corresponding latched mode-2 counters.
    constexpr std::array<std::array<std::uint64_t,2>,6> cases{{
        {48,0}, {49,1}, {239,4}, {240,5}, {1073304,22363}, {4416000,92014}
    }};
    std::uint64_t frames = 0;
    for (const auto& sample : cases) {
        while (frames < sample[0]) {
            const auto previous = timer.timer_milliseconds();
            timer.elapse_frame(produce);
            ++frames;
            constexpr auto work_per_frame = swiv3d::detail::CompatibilityClock::cpu_frequency / swiv3d::detail::device_audio_rate;
            require(timer.timer_milliseconds(work_per_frame) == previous,
                    "Pre-copy timestamp used the end of its device sample");
        }
        require(timer.timer_milliseconds() == sample[1],"Miles timer lost its PIT interpolation or requested-period carry");
    }
}

void test_device_audio() {
    swiv3d::detail::DeviceChannel sb(11025,swiv3d::detail::DeviceFilter::sound_blaster);
    const auto filtered = sb.next([] { return swiv3d::detail::StereoSample{-25856,-25856}; });
    require(swiv3d::detail::device_sum(filtered,{}) == swiv3d::detail::StereoSample{-10837,-10837},
            "SB filter lost fixed-point precision before the stereo sum");
    const swiv3d::detail::DeviceSample peaks{32767*8192,-32768*8192};
    require(swiv3d::detail::device_sum(peaks,peaks) == swiv3d::detail::StereoSample{32767,-32768},
            "CD/effects sum did not saturate after channel filtering");
    swiv3d::detail::Audio whole(bank()), split(bank());
    const std::vector<swiv3d::SoundEvent> loop{{0,swiv3d::no_entity,1000,swiv3d::SoundAction::loop}};
    const auto source = [](int) { return swiv3d::detail::SoundSource{}; };
    whole.present(loop,{},source); split.present(loop,{},source);
    const auto expected = whole.mix_device(9600);
    std::vector<std::int16_t> actual;
    for (const auto frames : {1,47,441,5000,4111}) {
        const auto part = split.mix_device(frames);
        actual.insert(actual.end(),part.begin(),part.end());
    }
    require(expected == actual,"Device filter/interpolation phase changed with host buffer size");
    const auto audible = std::find_if(expected.begin()+2000,expected.end(),[](auto value) { return std::abs(value) > 1; });
    require(audible >= expected.begin()+4096*2 && audible < expected.begin()+5000*2,
            "Device startup bypassed the silent DMA half");
    swiv3d::detail::CompatibilityClock clock(swiv3d::detail::device_audio_rate);
    for (int i = 0; i < 1000; ++i) { clock.elapse(1000000); }
    require(clock.drain_audio() == 48000,"Device output clock did not retain fractional 48-kHz timing");
}

void test_cd_device_restart() {
    swiv3d::detail::DeviceChannel channel(44100,swiv3d::detail::DeviceFilter::none);
    int value = 1000;
    unsigned reads = 0;
    const auto source = [&] { ++reads; return swiv3d::detail::StereoSample{static_cast<std::int16_t>(value),0}; };
    channel.next(source);
    channel.disable();
    value = 2000;
    for (unsigned frame = 0; frame < 2; ++frame) {
        require(swiv3d::detail::device_sum({},channel.next(source))[0] == 1000,"CD restart discarded the retained interpolation tail");
    }
    require(reads == 2,"CD disable did not reset the consumption phase independently of interpolation");
    require(swiv3d::detail::device_sum({},channel.next(source))[0] == 1837,"CD restart resumed with the wrong interpolation phase");
}

void test_sb16_probe() {
    using namespace swiv3d::detail;
    auto channel = DeviceChannel::sound_blaster(11025);
    for (unsigned frame = 0; frame < device_audio_rate; ++frame) {
        require(device_sum(channel.silence(),{}) == StereoSample{-4,-4},
                "Idle SB channel discarded its one-sample detection probe");
    }
    const auto zero = [] { return StereoSample{}; };
    require(device_sum(channel.next(zero),{}) == StereoSample{-13683,-13683},
            "DMA startup did not interpolate from the retained probe DAC");
    for (unsigned frame = 0; frame < 100; ++frame) { channel.next(zero); }
    require(device_sum(channel.silence(),{}) == StereoSample{-1,-1},
            "SB idle padding discarded the fixed-point filter tail");
}

void test_movie_driver() {
    swiv3d::detail::MovieSound sound;
    sound.open({22050,2,std::vector<std::int16_t>(47232,4096),8},71000);
    require(sound.service(1000) && sound.service(1001) && !sound.service(1002),
            "Initial sample-buffer readiness did not return zero, one, then unavailable");
    require(sound.state().available == 22656 && sound.state().queued == 24576 &&
            sound.state().lengths == std::array<std::uint32_t,2>{12288,12288} && sound.state().epoch == 1000,
            "Movie two-buffer ownership or initial timestamp differs");
    require(sound.played(1557) == 24563 && sound.played(1558) == 24576 && !sound.state().epoch,
            "Starved movie clock did not clamp to queued bytes");
    std::vector<std::int32_t> buffer(2048);
    for (unsigned refill = 0; refill < 4; ++refill) { sound.merge(buffer,127); }
    require(sound.state().current == 1 && sound.service(1600) && sound.state().completed == 24576 &&
            sound.state().queued == 12288 && sound.state().epoch == 1600,
            "Refill after clock starvation did not establish the next timestamp epoch");
    sound.close();
    require(!sound.service(1700) && !sound.state().playing,"Released movie retained a serviceable sample handle");
    sound.open({22050,2,{0,0,0,0},8},71000);
    require(!sound.service(1800),"Subminimum source data bypassed the refill threshold");
    sound.append({});
    require(sound.service(1801) && sound.state().lengths[0] == 4,
            "End-of-stream did not release its aligned short tail");
    swiv3d::detail::Audio buffered(bank());
    constexpr unsigned source_buffer_samples = 12288;
    std::vector<std::int16_t> pending(source_buffer_samples * 3,4096);
    std::fill(pending.begin() + source_buffer_samples * 2,pending.end(),-30000);
    buffered.begin_movie({22050,2,std::move(pending),8});
    const auto unserviced = buffered.mix_device(48000);
    require(*std::min_element(unserviced.begin() + 40000,unserviced.end()) >= -256,
            "Movie mixer consumed decoded data beyond its two loaded buffers");
    swiv3d::detail::Audio audio(bank());
    audio.begin_movie({11025,1,std::vector<std::int16_t>(4096,32767),16});
    const auto first = audio.mix_device(12000);
    require(std::all_of(first.begin()+200,first.begin()+8000,[](auto value) { return value == -1; }),"Movie PCM bypassed DMA startup buffering or discarded the probe tail");
    require(*std::max_element(first.begin(),first.end()) > 32000,"Movie PCM did not use the original full sample volume");
    audio.append_movie(std::vector<std::int16_t>(4096,-32768));
    const auto continuation = audio.mix_device(30000);
    require(*std::min_element(continuation.begin(),continuation.end()) < -32000,"Movie PCM continuation did not reach the shared mixer");
    audio.end_movie();
    audio.mix_device(48000);
    require((audio.voices()[0].flags & 1) == 0,"Movie handle consumed a game sound channel");
}

void test_dma_service() {
    auto channel = swiv3d::detail::DeviceChannel::sound_blaster(11025);
    require(channel.source_frames_needed(22) == 7,"Initial mixer demand lost its lookahead");
    channel.prefetch(7,[] { return swiv3d::detail::StereoSample{}; });
    for (unsigned frame = 0; frame < 22; ++frame) { channel.silence(); }
    require(channel.source_frames_needed(48) == 11,"Mixer demand discarded prefetched output");
    swiv3d::detail::SoundBlasterDma dma;
    unsigned builds = 0;
    dma.start();
    const auto produce = [&](auto& block) {
        ++builds;
        for (auto& sample : block) { sample = {123,-456}; }
        return true;
    };
    for (unsigned frame = 0; frame < 239; ++frame) { dma.elapse_frame(produce); }
    require(builds == 0,"DMA half filled before the first driver service");
    dma.elapse_frame(produce);
    require(builds == 1,"First driver service did not prepare the inactive DMA half");
    for (unsigned frame = 0; frame < dma.block_frames; ++frame) {
        require(dma.next() == swiv3d::detail::StereoSample{},"Startup DMA half was not silent");
    }
    require(dma.interrupts() == 1 && dma.next() == swiv3d::detail::StereoSample{123,-456},
            "SB interrupt did not advance to the prepared half");
    for (unsigned frame = 0; frame < 240; ++frame) { dma.elapse_frame(produce); }
    require(builds == 2,"IRQ0 did not refill the half released by the SB IRQ");
    const auto empty = [](auto& block) {
        block.fill({});
        return false;
    };
    for (unsigned round = 0; round < 3; ++round) {
        const auto remaining = round == 0 ? dma.block_frames-1 : dma.block_frames;
        for (std::size_t frame = 0; frame < remaining; ++frame) { dma.next(); }
        if (round == 2) {
            for (unsigned frame = 0; frame < 13; ++frame) { dma.next(); }
        }
        for (unsigned frame = 0; frame < 240; ++frame) { dma.elapse_frame(empty); }
    }
    require(dma.playing(),"Driver stop halted DMA before the current half drained");
    auto draining = dma;
    for (std::size_t frame = 0; frame < dma.block_frames - 13; ++frame) { draining.next(); }
    require(!draining.playing(),"Pending DMA stop missed the next half boundary");
    dma.start();
    require(dma.playing() && dma.next() == swiv3d::detail::StereoSample{},"DMA restart lost the cleared buffer state");
    for (unsigned frame = 0; frame < 240; ++frame) { dma.elapse_frame(produce); }
    require(builds == 2,"DMA restart discarded the retained last-serviced half");
    const auto interrupts = dma.interrupts();
    for (std::size_t frame = 0; frame < dma.block_frames - 14; ++frame) { dma.next(); }
    require(dma.interrupts() == interrupts + 1,"DMA restart reset the inherited transfer cursor");
}

void test_interrupted_transfer() {
    using namespace swiv3d::detail;
    swiv3d::MissionFrame old, frame;
    old.pixels.assign(swiv3d::image_width * swiv3d::image_height,1);
    frame.pixels.assign(old.pixels.size(),2);
    constexpr std::uint64_t start_ns = 12000000;
    constexpr std::uint64_t begin = start_ns * RasterSchedule::cycles_per_ms / 1000000;
    RasterSchedule reference;
    const auto copy = reference.copy(begin,1);
    const auto& transfer = copy.transfers.at(10);
    const auto service_begin = transfer.cycle - transfer.dwords - 1;
    const CpuServiceWindow service{service_begin,service_begin + RasterSchedule::cycles_per_ms};
    VideoPresenter uninterrupted(old,VideoPhase{start_ns,0,0,1});
    VideoPresenter interrupted(old,VideoPhase{start_ns,0,0,1},RasterSchedule({}, {service}));
    uninterrupted.begin_frame(frame); interrupted.begin_frame(frame);
    uninterrupted.elapse(1200000); interrupted.elapse(1200000);
    require(uninterrupted.displayed().pixels == frame.pixels && interrupted.copying() &&
            interrupted.displayed().pixels == old.pixels,"CPU service did not suspend the copy across the VGA latch");
    interrupted.elapse(1000000);
    require(!interrupted.copying() && interrupted.transferred_pixels() == frame.pixels.size(),
            "Interrupted REP lost pixels or failed to resume");
    interrupted.elapse(14000000);
    require(interrupted.displayed().pixels == frame.pixels,"Interrupted copy missed the next complete-page latch");
}

void test_generated_timer_service() {
    using namespace swiv3d::detail;
    TimerService timer;
    for (unsigned irq = 1; irq <= 11; ++irq) {
        const auto expected = 698u + (irq % 5 == 0 ? 71u : 0u) + (irq == 11 ? 706u : 0u);
        require(timer.interrupt() == expected,"Native timer shell lost its message or BIOS phase");
    }
    const auto begin = static_cast<std::uint64_t>((RasterPhase::pit_ms - .004) * RasterSchedule::cycles_per_ms);
    RasterSchedule external, generated({}, {}, IrqScheduling::timer_shell);
    const auto expected = external.copy(begin,1);
    const auto actual = generated.copy(begin,1);
    require(actual.request_cycle > expected.request_cycle,
            "Live IRQ shell did not suspend its own planar transfer");
    unsigned dwords = 0;
    for (const auto& transfer : actual.transfers) { dwords += transfer.dwords; }
    require(dwords * 4 == swiv3d::image_width * swiv3d::image_height,
            "Live IRQ shell lost pixels while resuming its transfer");
}

void test_device_irq_schedule() {
    using namespace swiv3d::detail;
    Audio audio(bank()), reference(bank());
    PcmSample movie{22050,2,std::vector<std::int16_t>(80000),8};
    for (std::size_t sample = 0; sample < movie.samples.size(); ++sample) {
        movie.samples[sample] = static_cast<std::int16_t>((static_cast<int>(sample % 256) - 128) * 256);
    }
    audio.begin_movie(movie);
    reference.begin_movie(movie);
    std::array<std::uint64_t,32> predicted{};
    for (std::size_t index = 0; index < predicted.size(); ++index) { predicted[index] = audio.irq_work(index + 1); }
    require(predicted[0] == 31356 && predicted[1] == 735,
            "Device IRQ forecast omitted original movie refill or retained-half work");
    require(audio.mix_device(12000) == reference.mix_device(12000),
            "IRQ forecast advanced live digital state");
    for (std::size_t index = 0; index < predicted.size(); ++index) {
        require(audio.irq_work(index + 1) == predicted[index],"Predicted IRQ cost differs from executed service");
    }
    audio.end_movie();
    reference.end_movie();
    constexpr std::uint64_t first_stopped_irq = 51;
    constexpr unsigned stopped_irq_stride = 2;
    for (std::size_t index = 0; index < predicted.size(); ++index) {
        predicted[index] = audio.irq_work(first_stopped_irq + index * stopped_irq_stride);
    }
    require(audio.irq_work(93) == 14479 && audio.irq_work(95) == 798,
            "Device IRQ forecast missed driver stop or subsequent idle service");
    require(audio.mix_device(16000) == reference.mix_device(16000),
            "Stopped-driver IRQ forecast advanced device playback");
    for (std::size_t index = 0; index < predicted.size(); ++index) {
        require(audio.irq_work(first_stopped_irq + index * stopped_irq_stride) == predicted[index],
                "Predicted stop/idle IRQ cost differs from executed service");
    }
    RasterSchedule shell({}, {}, IrqScheduling::timer_shell);
    RasterSchedule device({}, {}, IrqScheduling::device,[&](auto interrupt) { return audio.irq_work(interrupt); });
    const auto begin = static_cast<std::uint64_t>((RasterPhase::pit_ms - .004) * RasterSchedule::cycles_per_ms);
    const auto shorter = shell.copy(begin,1);
    const auto longer = device.copy(begin,1);
    require(longer.request_cycle > shorter.request_cycle + 30000,
            "Native digital refill work did not suspend the planar transfer");
    unsigned dwords = 0;
    for (const auto& transfer : longer.transfers) { dwords += transfer.dwords; }
    require(dwords * 4 == swiv3d::image_width * swiv3d::image_height,
            "Device IRQ service lost planar transfer data");
    RasterSchedule crossing({}, {}, IrqScheduling::device,[&](auto interrupt) { return audio.irq_work(interrupt); });
    const auto during = static_cast<std::uint64_t>((RasterPhase::pit_ms + .010) * RasterSchedule::cycles_per_ms);
    require(crossing.copy(during,1).port_cycles[0] > during + 30000,
            "Copy beginning inside digital service discarded the remaining IRQ work");
    RasterSchedule idle;
    require(idle.idle(0,1) > 13,"Disk idle callback retained the discarded CPU budget");
    RasterPhase boundary;
    boundary.events.fill(std::numeric_limits<double>::infinity());
    RasterSchedule terminal(boundary);
    require(terminal.idle(RasterSchedule::cycles_per_ms - 13,0) == RasterSchedule::cycles_per_ms,
            "Idle return callback charged an ordinary CPU-budget exit");
}

void test_planar_transfer() {
    swiv3d::MissionFrame old, frame;
    old.pixels.assign(swiv3d::image_width * swiv3d::image_height,1);
    frame.pixels.assign(old.pixels.size(),2);
    swiv3d::detail::VideoPresenter video(old);
    video.begin_frame(frame);
    video.elapse(800);
    require(video.transferred_pixels() == 0,"VGA transfer bypassed its register setup");
    video.elapse(84);
    require(video.transferred_pixels() == 16,"VGA transfer did not preserve four-dword REP granularity");
    require(video.displayed().pixels == old.pixels,"Back-page transfer changed the scanned page");
    video.elapse(1000000);
    require(!video.copying() && video.transferred_pixels() == frame.pixels.size(),"VGA planes did not finish before the page request");
    video.elapse(14000000);
    require(video.displayed().pixels == frame.pixels,"Completed VGA page did not latch on retrace");
    swiv3d::detail::VideoPresenter late(old);
    late.elapse(13000000);
    late.begin_frame(frame);
    late.elapse(1000000);
    require(late.displayed().pixels == old.pixels,"Page latched before its planar copy finished");
    late.elapse(14000000);
    require(late.displayed().pixels == frame.pixels,"Late transfer did not use the following retrace");
    swiv3d::detail::RasterSchedule schedule;
    const auto copy = schedule.copy(0,1);
    const auto request_ns = (copy.request_cycle * UINT64_C(1000000000) + 59999999) / 60000000;
    swiv3d::detail::VideoPresenter pending(old);
    pending.begin_frame(frame);
    pending.elapse(request_ns - 1);
    require(pending.copying() && pending.transferred_pixels() == frame.pixels.size(),
            "CRTC page request bypassed its ISA OUT delay");
    pending.elapse(1);
    require(!pending.copying(),"CRTC page request missed its scheduled completion");
    const auto latch_ns = (swiv3d::detail::VideoClock::retrace_dot * UINT64_C(1000000000) +
                          swiv3d::detail::VideoClock::dot_frequency - 1) / swiv3d::detail::VideoClock::dot_frequency;
    std::uint64_t start_ns = latch_ns - request_ns;
    std::uint64_t finish_ns = 0;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        swiv3d::detail::RasterSchedule phase;
        const auto begin = start_ns * 60000000 / 1000000000;
        const auto timed_copy = phase.copy(begin,1);
        finish_ns = (timed_copy.request_cycle * UINT64_C(1000000000) + 59999999) / 60000000;
        if (attempt != 2) { start_ns = start_ns + latch_ns + 250 - finish_ns; }
    }
    swiv3d::detail::VideoPresenter atomic(old);
    atomic.elapse(start_ns);
    atomic.begin_frame(frame);
    atomic.elapse(finish_ns - start_ns);
    require(atomic.displayed().pixels == frame.pixels,"VGA latch ran inside an unfinished CRTC OUT instruction");
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--dsp-clock") {
            swiv3d::detail::DspBusyClock dsp;
            std::ifstream input(argv[2]);
            char operation;
            std::uint64_t cycle;
            while (input >> operation >> cycle) {
                if (operation == 'R') { std::cout << dsp.busy(cycle) << '\n'; }
                else if (operation == 'W') { dsp.write(cycle); }
                else if (operation == 'C') { dsp.complete(); }
                else { throw std::runtime_error("Invalid DSP clock operation"); }
            }
            require(input.eof(),"Invalid DSP clock fixture");
            return 0;
        }
        if (argc == 4 && std::string(argv[1]) == "--movie-sound") {
            movie_sound_fixture(argv[2],argv[3]);
            return 0;
        }
        test_decode_and_lifetime(); test_channel_order_and_spatial_delay(); test_pit_and_frame_counters();
        test_cd_track_boundaries();
        test_digital_merge_contract();
        test_master_gain_before_clipping();
        test_video_retrace_latch();
        test_observed_pit_terminal_count();
        test_miles_timer();
        test_device_audio();
        test_planar_transfer();
        test_interrupted_transfer();
        test_generated_timer_service();
        test_device_irq_schedule();
        test_dma_service();
        test_cd_device_restart();
        test_sb16_probe();
        test_movie_driver();
        test_startup_scheduling();
        std::cout << "PCM, sound channels, spatial delay, PIT and frame counters passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
