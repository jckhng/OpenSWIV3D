#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#include "audio_output.hpp"
#include "audio.hpp"

#include <array>
#include <stdexcept>

namespace swiv3d::host {
class AudioOutput::Impl {
public:
    Impl() {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec = detail::device_audio_rate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        if (waveOutOpen(&device_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            throw std::runtime_error("Cannot open native audio output");
        }
    }

    ~Impl() {
        waveOutReset(device_);
        for (auto& buffer : buffers_) {
            if ((buffer.header.dwFlags & WHDR_PREPARED) != 0) { waveOutUnprepareHeader(device_, &buffer.header, sizeof(WAVEHDR)); }
        }
        waveOutClose(device_);
    }

    void submit(const std::vector<std::int16_t>& samples) {
        if (samples.empty()) { return; }
        auto& buffer = buffers_[next_];
        if ((buffer.header.dwFlags & WHDR_PREPARED) != 0) {
            while ((buffer.header.dwFlags & WHDR_DONE) == 0) { Sleep(1); }
            if (waveOutUnprepareHeader(device_, &buffer.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                throw std::runtime_error("Cannot release native audio buffer");
            }
        }
        buffer.samples = samples;
        buffer.header = {};
        buffer.header.lpData = reinterpret_cast<LPSTR>(buffer.samples.data());
        buffer.header.dwBufferLength = static_cast<DWORD>(buffer.samples.size() * sizeof(std::int16_t));
        if (waveOutPrepareHeader(device_, &buffer.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
            waveOutWrite(device_, &buffer.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            throw std::runtime_error("Cannot submit native audio buffer");
        }
        next_ = (next_ + 1) % buffers_.size();
    }

private:
    struct Buffer { WAVEHDR header{}; std::vector<std::int16_t> samples; };
    HWAVEOUT device_ = nullptr;
    std::array<Buffer, 16> buffers_{};
    std::size_t next_ = 0;
};

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}
AudioOutput::~AudioOutput() = default;
void AudioOutput::submit(const std::vector<std::int16_t>& samples) { impl_->submit(samples); }
}
