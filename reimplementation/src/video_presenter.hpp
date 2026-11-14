#pragma once

#include "video_clock.hpp"
#include "raster_schedule.hpp"
#include "swiv3d/mission.hpp"

#include <algorithm>

namespace swiv3d::detail {

struct VideoPhase {
    std::uint64_t nanoseconds = 0;
    unsigned displayed_page = 0, requested_page = 0, next_page = 1;
};

// Pinned normal-core profile. REP is interruptible every four dwords. Each
// dword takes one cycle and each REP dispatch takes one additional cycle.
class VideoPresenter {
public:
    explicit VideoPresenter(const MissionFrame& initial, RasterSchedule schedule = RasterSchedule{})
        : schedule_(std::move(schedule)) { pages_[0] = initial; }

    VideoPresenter(const MissionFrame& initial, VideoPhase phase, RasterSchedule schedule = RasterSchedule{})
        : VideoPresenter(initial,std::move(schedule)) {
        if (phase.next_page > 1) { throw std::runtime_error("Invalid next VGA page"); }
        pages_[1] = initial;
        clock_.request_page(phase.displayed_page);
        clock_.elapse((VideoClock::dots_per_frame * nanoseconds_per_second + VideoClock::dot_frequency - 1) /
                      VideoClock::dot_frequency + phase.nanoseconds);
        clock_.request_page(phase.requested_page);
        next_page_ = phase.next_page;
        time_ns_ = phase.nanoseconds;
    }

    void begin_frame(const MissionFrame& frame) {
        if (copying_) { throw std::runtime_error("VGA transfer is still pending"); }
        if (frame.pixels.size() != image_width * image_height) { throw std::runtime_error("Invalid VGA frame size"); }
        source_ = frame.pixels;
        pages_[next_page_].palette = frame.palette;
        pages_[next_page_].pixels.resize(source_.size());
        cycles_ = copied_ = 0;
        fraction_ = time_ns_ % nanoseconds_per_second * cpu_frequency % nanoseconds_per_second;
        transfer_ = 0;
        const auto begin = time_ns_ / nanoseconds_per_second * cpu_frequency +
            time_ns_ % nanoseconds_per_second * cpu_frequency / nanoseconds_per_second;
        plan_ = schedule_.copy(begin,next_page_);
        for (auto& transfer : plan_.transfers) { transfer.cycle -= begin; }
        plan_.request_cycle -= begin;
        const auto out_cycle = plan_.port_cycles[4] - 1 - begin;
        out_start_ns_ = (out_cycle * nanoseconds_per_second - fraction_ + cpu_frequency - 1) / cpu_frequency;
        copy_ns_ = held_ns_ = 0;
        copying_ = true;
    }

    void elapse(std::uint64_t nanoseconds) {
        time_ns_ += nanoseconds;
        if (!copying_) { clock_.elapse(nanoseconds); return; }
        const auto completion = plan_.request_cycle;
        const auto remaining = (completion - cycles_) * nanoseconds_per_second - fraction_;
        const auto until_complete = (remaining + cpu_frequency - 1) / cpu_frequency;
        const auto elapsed = std::min(nanoseconds,until_complete);
        const auto partial = elapsed * cpu_frequency + fraction_;
        cycles_ += partial / nanoseconds_per_second;
        fraction_ = partial % nanoseconds_per_second;
        const auto clock_elapsed = std::min(copy_ns_ + elapsed,out_start_ns_) - std::min(copy_ns_,out_start_ns_);
        copy_ns_ += elapsed;
        held_ns_ += elapsed - clock_elapsed;
        clock_.elapse(clock_elapsed);
        transfer();
        if (cycles_ < completion) { return; }
        clock_.request_page(next_page_);
        // PIC callbacks due inside OUT run after its register write. Retain
        // their elapsed time until the atomic ISA operation has completed.
        clock_.elapse(held_ns_);
        next_page_ ^= 1;
        copying_ = false;
        clock_.elapse(nanoseconds - elapsed);
    }

    const MissionFrame& displayed() const { return pages_[clock_.display_page()]; }
    std::uint64_t retraces() const { return clock_.retraces(); }
    bool copying() const { return copying_; }
    std::size_t transferred_pixels() const { return copied_; }
    unsigned display_page() const { return clock_.display_page(); }

private:
    void transfer() {
        std::size_t ready = 0;
        while (transfer_ < plan_.transfers.size() && plan_.transfers[transfer_].cycle <= cycles_) {
            ready += plan_.transfers[transfer_++].dwords * 4;
        }
        ready += copied_;
        auto& pixels = pages_[next_page_].pixels;
        while (copied_ < ready) {
            const auto plane = copied_ / pixels_per_plane;
            const auto offset = (copied_ % pixels_per_plane) * 4 + plane;
            pixels[offset] = source_[offset];
            ++copied_;
        }
    }

    static constexpr std::uint64_t nanoseconds_per_second = 1000000000;
    static constexpr std::uint64_t cpu_frequency = 60000000;
    static constexpr std::size_t pixels_per_plane = image_width * image_height / 4;
    VideoClock clock_;
    RasterSchedule schedule_;
    PlanarCopy plan_;
    std::array<MissionFrame,2> pages_;
    std::vector<std::uint8_t> source_;
    std::uint64_t cycles_ = 0, fraction_ = 0, time_ns_ = 0;
    std::uint64_t out_start_ns_ = 0, copy_ns_ = 0, held_ns_ = 0;
    std::size_t copied_ = 0, transfer_ = 0;
    unsigned next_page_ = 1;
    bool copying_ = false;
};

}
