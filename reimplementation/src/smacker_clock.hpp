#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace swiv3d::detail {

// Smacker increments its audio deadline in 1/128-byte units. Miles reports
// bytes played using the integer-millisecond timer and the source byte rate.
class SmackerClock {
public:
    SmackerClock(std::uint32_t duration_us, std::uint32_t bytes_per_second)
        : duration_(duration_us), rate_(bytes_per_second),
          step_(std::uint64_t(rate_) * duration_ * byte_fraction / 1000000), deadline_(step_) {
        if (duration_ == 0) { throw std::runtime_error("Invalid movie frame duration"); }
    }

    void elapse(std::uint32_t microseconds) { elapsed_ += microseconds; }

    bool ready() const {
        if (rate_ == 0) { return elapsed_ >= duration_; }
        const auto played = (elapsed_ / 1000) * rate_ / 1000;
        return played >= deadline_ / byte_fraction;
    }

    bool ready(std::uint32_t played_bytes) const { return played_bytes >= deadline_ / byte_fraction; }

    void limit_audio(std::uint64_t decoded_bytes) {
        if (rate_ != 0) { deadline_ = std::min(deadline_,decoded_bytes * byte_fraction); }
    }

    void next() {
        if (rate_ == 0) { elapsed_ -= duration_; }
        else { deadline_ += step_; }
    }

private:
    static constexpr unsigned byte_fraction = 128;
    std::uint32_t duration_, rate_;
    std::uint64_t step_, deadline_, elapsed_ = 0;
};

}
