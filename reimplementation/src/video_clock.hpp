#pragma once

#include <cstdint>
#include <stdexcept>

namespace swiv3d::detail {

// Recovered 320x400 VGA timing. The CRTC start address latches at vertical
// retrace, independently of the PIT-driven game callbacks and frame rendering.
class VideoClock {
public:
    static constexpr std::uint64_t dot_frequency = 25175000;
    static constexpr std::uint64_t dots_per_line = 800;
    static constexpr std::uint64_t lines_per_frame = 449;
    static constexpr std::uint64_t retrace_line = 412;
    static constexpr std::uint64_t dots_per_frame = dots_per_line * lines_per_frame;
    static constexpr std::uint64_t retrace_dot = dots_per_line * retrace_line;

    void elapse(std::uint64_t nanoseconds) {
        constexpr std::uint64_t nanoseconds_per_second = 1000000000;
        const auto partial = (nanoseconds % nanoseconds_per_second) * dot_frequency + fraction_;
        const auto dots = nanoseconds / nanoseconds_per_second * dot_frequency + partial / nanoseconds_per_second;
        fraction_ = partial % nanoseconds_per_second;
        const auto phase = (position_ + dots_per_frame - retrace_dot) % dots_per_frame;
        const auto retraces = (phase + dots) / dots_per_frame;
        position_ = (position_ + dots) % dots_per_frame;
        if (retraces == 0) { return; }
        retraces_ += retraces;
        display_page_ = requested_page_;
    }

    void request_page(unsigned page) {
        if (page > 1) { throw std::runtime_error("VGA page exceeds double-buffer storage"); }
        requested_page_ = page;
    }

    unsigned display_page() const { return display_page_; }
    std::uint64_t retraces() const { return retraces_; }

private:
    std::uint64_t position_ = 0, fraction_ = 0, retraces_ = 0;
    unsigned requested_page_ = 0, display_page_ = 0;
};

}
