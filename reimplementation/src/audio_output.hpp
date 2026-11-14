#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace swiv3d::host {
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();
    void submit(const std::vector<std::int16_t>& samples);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
