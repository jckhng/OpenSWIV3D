#include "cutscene.hpp"
#include "alpha.hpp"
#include "audio.hpp"
#include "combat_math.hpp"
#include "smacker_work.hpp"
#include "smacker_loading.hpp"
#include "smacker_stream.hpp"
#include "smacker_clock.hpp"
#include "../third_party/libsmacker/smacker.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>

namespace swiv3d::detail {
namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw std::runtime_error("Cannot open cutscene " + path.string()); }
    return {std::istreambuf_iterator<char>(input), {}};
}

std::size_t lookup(const std::vector<std::uint8_t>& strings, std::size_t table, std::uint32_t id) {
    for (auto offset = table; offset + 8 <= strings.size(); offset += 8) {
        const auto key = static_cast<std::uint32_t>(combat_math::word(strings, offset));
        if (key == id) { return static_cast<std::size_t>(combat_math::word(strings, offset + 4)); }
        if (key == UINT32_MAX) { break; }
    }
    throw std::runtime_error("Missing cutscene string");
}

std::string text(const std::vector<std::uint8_t>& strings, unsigned group, unsigned id, unsigned language) {
    const auto offset = lookup(strings, lookup(strings, 0, group + (language << 16)), id);
    auto end = offset;
    while (strings.at(end) != 0) { ++end; }
    return {strings.begin() + offset, strings.begin() + end};
}

struct Caption {
    std::string text;
    unsigned first, last;
    int x, y;
    char colour;
};

}

class Cutscene::Impl {
    struct FirstFrameCaller {
        std::uint64_t before_audio = 0, after_audio = 0;
    };
public:
    ~Impl() { if (audio_) { audio_->end_movie(); } }

    void attach_audio(Audio& audio) {
        if (audio_) { throw std::runtime_error("Movie audio is already attached"); }
        if (tracks_ != 0) { audio.begin_movie({static_cast<std::uint32_t>(rates_[0]),channels_[0],pcm_,depth_[0]},duration_,prepared_audio_epoch_); }
        audio_ = &audio;
        if (tracks_ != 0) { clock_->limit_audio(audio_->movie_sound_state().decoded); }
    }

    void initialize_audio(Audio& audio) {
        if (first_frame_caller().before_audio == 0) { attach_audio(audio); return; }
        if (audio_) { throw std::runtime_error("Movie audio is already attached"); }
        MoviePreparation startup;
        constexpr unsigned after_wait_work = 2578, animation_entry_work = 318;
        constexpr unsigned before_palette_work = 4766;
        startup.cpu(after_wait_work + animation_entry_work);
        const auto plan = preparation();
        const auto& stages = plan.stages();
        for (std::size_t index = 0; index < stages.size(); ++index) {
            const auto& stage = stages[index];
            if (stage.kind == PreparationKind::disk) { startup.disk(stage.work); }
            else { startup.cpu(stage.work + (index + 1 == stages.size() ? before_palette_work : 0)); }
        }
        const auto driver_tail = audio.begin_opening(
            {static_cast<std::uint32_t>(rates_[0]),channels_[0],pcm_,depth_[0]},duration_,startup);
        audio_ = &audio;
        clock_->limit_audio(audio_->movie_sound_state().decoded);
        playback_work_.cpu(driver_tail + first_video_work());
    }

    Impl(const std::filesystem::path& directory, const std::string& name,
         const Archive& globals, const FrontendState& request)
        : bytes_(read_file(directory / (name + ".SMK"))), font_(globals.load(507)),
          widths_(globals.load(508)), alpha_(globals.load(800)) {
        movie_.reset(smk_open_memory(bytes_.data(), static_cast<unsigned long>(bytes_.size())));
        if (!movie_) { throw std::runtime_error("Cannot decode Smacker cutscene " + name); }
        unsigned long width = 0, height = 0, count = 0;
        unsigned char scale = 0;
        double duration = 0;
        smk_info_video(movie_.get(), &width, &height, &scale);
        smk_info_all(movie_.get(), nullptr, &count, &duration);
        if (width != image_width || height != image_height || scale != SMK_FLAG_Y_NONE || count == 0 || duration <= 0) {
            throw std::runtime_error("Unsupported cutscene dimensions or timing");
        }
        frames_ = static_cast<std::uint32_t>(count);
        duration_ = static_cast<std::uint32_t>(duration);
        const auto strings = globals.load(100);
        const auto pages = text(strings, 2, 0, 99);
        for (unsigned character = 0; character < glyphs_.size(); ++character) {
            glyphs_[character] = character == 127 ? 0 : character < 128 ? character :
                character + static_cast<unsigned>(pages.at(request.language) - '0') * 128;
        }
        std::ifstream script(directory / (name + ".SCR"));
        unsigned group, id, first, last;
        int x, y;
        char colour, align;
        while (script >> group >> id >> first >> last >> x >> y >> colour >> align) {
            if (group == 200) { id = request.level; }
            auto value = text(strings, group, id, request.language);
            int text_width = 0;
            std::size_t length = 0;
            constexpr int caption_width_limit = 280;
            while (length < value.size() && text_width < caption_width_limit) {
                text_width += combat_math::word(widths_, glyphs_[static_cast<unsigned char>(value[length++])] * 4);
            }
            value.resize(length);
            if (align == 'c') { x -= text_width >> 1; }
            if (align == 'r') { x -= text_width; }
            captions_.push_back({std::move(value), first, last, x, y, colour});
        }
        smk_info_audio(movie_.get(), &tracks_, channels_.data(), depth_.data(), rates_.data());
        if ((tracks_ & ~SMK_AUDIO_TRACK_0) != 0) { throw std::runtime_error("Unsupported cutscene audio tracks"); }
        clock_.emplace(duration_,tracks_ == 0 ? 0 :
            static_cast<std::uint32_t>(rates_[0] * channels_[0] * depth_[0] / 8));
        smk_enable_all(movie_.get(), SMK_VIDEO_TRACK | tracks_);
        if (smk_first(movie_.get()) == SMK_ERROR) { throw std::runtime_error("Cannot decode first cutscene frame"); }
        decoded();
        first_pcm_bytes_ = pcm_.size() * depth_[0] / 8;
        stream_.emplace(bytes_);
    }

    bool advance(std::uint32_t microseconds) {
        if (finished_) { return true; }
        if (audio_) { audio_->service_movie(); }
        elapsed_ += microseconds;
        clock_->elapse(microseconds);
        while (elapsed_ >= duration_) {
            if (next_frame()) { return true; }
        }
        return false;
    }

    bool advance_device(std::size_t frames, MoviePolling polling) {
        if (finished_) { return true; }
        device_fraction_ += std::uint64_t(frames) * 1000000;
        const auto microseconds = static_cast<std::uint32_t>(device_fraction_ / device_audio_rate);
        device_fraction_ %= device_audio_rate;
        elapsed_ += microseconds;
        if (!audio_ || tracks_ == 0) { clock_->elapse(microseconds); }
        if (polling == MoviePolling::suspended) { return false; }
        if (audio_) {
            if (playback_work_.remaining() != 0) {
                const auto wall = std::uint64_t(frames) * CompatibilityClock::cpu_frequency / device_audio_rate;
                playback_work_.elapse_device(wall,audio_->cpu_work(),audio_->device_cycles() - wall,
                    [audio = audio_](auto interrupt) { return audio->irq_work(interrupt); });
                if (playback_work_.remaining() == 0 && decoding_) {
                    decoding_ = false;
                    return next_frame();
                }
                return false;
            }
            audio_->service_movie();
            if (!preloaded_) {
                playback_work_.reset(stream_->next());
                preloaded_ = true;
                if (playback_work_.remaining() != 0) { return false; }
            }
        }
        while (audio_ && tracks_ != 0 ? clock_->ready(audio_->movie_bytes_played()) : clock_->ready()) {
            if (audio_ && frame_ < frames_) {
                playback_work_.reset({});
                playback_work_.cpu(smacker_work::pcm_work(bytes_,frame_));
                if (playback_work_.remaining() != 0) { decoding_ = true; return false; }
            }
            if (next_frame()) { return true; }
            if (audio_) { return false; }
        }
        if (audio_) { playback_work_.reset(stream_->background()); }
        return false;
    }

    std::size_t device_frames_until_poll() const {
        // Miles interpolates between PIT interrupts. Observe its integer clock
        // at each device sample, independently of the host output partition.
        return 1;
    }

    MissionFrame render() const {
        auto result = image_;
        AlphaPresenter presenter(result.pixels, alpha_, AlphaDestination::video);
        for (const auto& caption : captions_) {
            if (frame_ < caption.first || frame_ > caption.last) { continue; }
            auto x = caption.x;
            for (unsigned char character : caption.text) {
                presenter.draw(font_, x, caption.y, glyphs_[character],
                               caption.colour == 'b' ? AlphaIntensity::full : AlphaIntensity::half);
                x += combat_math::word(widths_, glyphs_[character] * 4);
            }
        }
        return result;
    }

    std::vector<std::int16_t> mix(std::size_t frames, std::uint32_t output_rate = audio_rate) {
        std::vector<std::int16_t> output(frames * 2);
        if (tracks_ == 0) { return output; }
        for (std::size_t frame = 0; frame < frames; ++frame, ++audio_position_) {
            const auto source = audio_position_ * rates_[0] / output_rate;
            for (unsigned channel = 0; channel < 2; ++channel) {
                const auto index = source * channels_[0] + channel % channels_[0];
                if (index < pcm_.size()) { output[frame * 2 + channel] = pcm_[index]; }
            }
        }
        return output;
    }

    std::uint32_t frame_ = 1, frames_ = 0, duration_ = 0;

    std::uint64_t first_video_work() const {
        return first_frame_caller().after_audio + smacker_work::Tables(bytes_).first_video_work();
    }

    MoviePreparation preparation() const {
        const auto reads = smacker_loading::opening_reads(bytes_);
        // The first hardware DAC upload precedes SmackDoFrame and audio activation.
        constexpr unsigned palette_upload_work = 38468;
        const auto count = std::uint64_t(static_cast<std::uint32_t>(combat_math::word(bytes_,12))) +
            (static_cast<std::uint32_t>(combat_math::word(bytes_,20)) & 1);
        const auto first_type = bytes_.at(static_cast<std::size_t>(104 + count * 4));
        return smacker_loading::preparation(bytes_,reads,smacker_work::Tables(bytes_).work(),
            smacker_work::first_pcm_work(bytes_) + first_frame_caller().before_audio +
            ((first_type & 1) != 0 ? palette_upload_work : 0));
    }

    void prepare_audio_clock(Audio& audio, std::uint64_t before, std::uint64_t after, std::uint64_t cpu_work) {
        // Original timer return through the first 12-KiB copy to driver start.
        constexpr unsigned clock_to_audio_work = 3965;
        if (prepared_audio_epoch_ || before < clock_to_audio_work || after > clock_to_audio_work) { return; }
        if (first_frame_caller().before_audio == 0) { return; }
        prepared_audio_epoch_ = audio.movie_timestamp(cpu_work - (before - clock_to_audio_work));
    }

private:
    FirstFrameCaller first_frame_caller() const {
        constexpr unsigned source_buffer_bytes = 12288;
        constexpr unsigned source_rate = 22050;
        if (tracks_ != SMK_AUDIO_TRACK_0 || rates_[0] != source_rate) { return {}; }
        const auto reads = smacker_loading::opening_reads(bytes_);
        if (first_pcm_bytes_ < 2 * source_buffer_bytes || smacker_loading::caller_parts(bytes_,reads).index == 0) { return {}; }
        // Protected caller around first PCM/video decoding. The driver-start
        // device callee has its own timing contract.
        constexpr unsigned before_pcm = 355, before_start = 4351;
        constexpr unsigned after_start = 283, after_video = 4294;
        return {before_pcm + before_start,after_start + after_video};
    }

    bool next_frame() {
        elapsed_ -= duration_;
        clock_->next();
        if (frame_ == frames_) { finished_ = true; return true; }
        if (stream_ && !preloaded_) { stream_->next(); }
        preloaded_ = false;
        if (smk_next(movie_.get()) == SMK_ERROR) { throw std::runtime_error("Cannot decode cutscene frame"); }
        ++frame_;
        decoded();
        return false;
    }

    void decoded() {
        const auto video = smk_get_video(movie_.get());
        image_.pixels.assign(video, video + image_width * image_height);
        const auto palette = smk_get_palette(movie_.get());
        for (std::size_t i = 0; i < image_.palette.size(); ++i) { image_.palette[i] = palette[i] >> 2; }
        std::fill_n(image_.palette.begin(), 3, std::uint8_t{0}); // PCSPEC_Anim_Pageflip_ clears DAC entry zero.
        if (tracks_ == 0) { return; }
        const auto size = smk_get_audio_size(movie_.get(), 0);
        const auto data = smk_get_audio(movie_.get(), 0);
        const auto previous_size = pcm_.size();
        for (unsigned long offset = 0; offset < size;) {
            if (depth_[0] == 8) { pcm_.push_back(static_cast<std::int16_t>((static_cast<int>(data[offset++]) - 128) * 256)); }
            else {
                const auto value = data[offset] | static_cast<unsigned>(data[offset + 1]) << 8;
                pcm_.push_back(static_cast<std::int16_t>(value));
                offset += 2;
            }
        }
        if (audio_) { audio_->append_movie({pcm_.begin()+previous_size,pcm_.end()}); }
        clock_->limit_audio(audio_ ? audio_->movie_sound_state().decoded : pcm_.size() * depth_[0] / 8);
    }

    std::vector<std::uint8_t> bytes_, font_, widths_, alpha_;
    std::unique_ptr<smk_t, decltype(&smk_close)> movie_{nullptr, smk_close};
    std::array<unsigned, 256> glyphs_{};
    std::vector<Caption> captions_;
    MissionFrame image_;
    std::optional<SmackerClock> clock_;
    std::optional<SmackerStream> stream_;
    MoviePreparation playback_work_;
    bool preloaded_ = false;
    bool decoding_ = false;
    std::int64_t elapsed_ = 0;
    std::uint64_t device_fraction_ = 0, audio_position_ = 0;
    std::uint64_t first_pcm_bytes_ = 0;
    bool finished_ = false;
    std::optional<std::uint32_t> prepared_audio_epoch_;
    unsigned char tracks_ = 0;
    std::array<unsigned char, 7> channels_{}, depth_{};
    std::array<unsigned long, 7> rates_{};
    std::vector<std::int16_t> pcm_;
    Audio* audio_ = nullptr;
};

Cutscene::Cutscene(const std::filesystem::path& directory, const std::string& name,
                   const Archive& globals, const FrontendState& request)
    : impl_(std::make_unique<Impl>(directory, name, globals, request)) {}
Cutscene::~Cutscene() = default;
bool Cutscene::advance(std::uint32_t microseconds) { return impl_->advance(microseconds); }
bool Cutscene::advance_device(std::size_t frames, MoviePolling polling) { return impl_->advance_device(frames,polling); }
std::size_t Cutscene::device_frames_until_poll() const { return impl_->device_frames_until_poll(); }
MissionFrame Cutscene::render() const { return impl_->render(); }
std::vector<std::int16_t> Cutscene::mix(std::size_t frames) { return impl_->mix(frames); }
std::vector<std::int16_t> Cutscene::mix_device(std::size_t frames) { return impl_->mix(frames,device_audio_rate); }
void Cutscene::attach_audio(Audio& audio) { impl_->attach_audio(audio); }
void Cutscene::initialize_audio(Audio& audio) { impl_->initialize_audio(audio); }
void Cutscene::prepare_audio_clock(Audio& audio, std::uint64_t before, std::uint64_t after, std::uint64_t cpu_work) {
    impl_->prepare_audio_clock(audio,before,after,cpu_work);
}
std::uint32_t Cutscene::frame() const { return impl_->frame_; }
std::uint32_t Cutscene::frames() const { return impl_->frames_; }
std::uint32_t Cutscene::frame_duration() const { return impl_->duration_; }
std::uint64_t Cutscene::first_video_work() const { return impl_->first_video_work(); }
std::uint64_t Cutscene::preparation_work() const { return impl_->preparation().remaining(); }
MoviePreparation Cutscene::preparation() const { return impl_->preparation(); }

}
