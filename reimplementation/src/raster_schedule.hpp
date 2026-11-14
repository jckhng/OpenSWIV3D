#pragma once

#include "clock.hpp"
#include "timer_service.hpp"
#include "video_clock.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace swiv3d::detail {

// Event phases belong to the compatibility profile. They do not depend on
// host display refresh or on a debugger stopping the guest CPU.
struct RasterPhase {
    enum Event { scanline, pit, vertical_irq, display_latch, panning, frame, rtc, event_count };
    static constexpr unsigned visible_lines = 400, retrace_end_line = 414;
    static constexpr double vertical_irq_delay_ms = .005;
    static constexpr double line_ms = VideoClock::dots_per_line * 1000.0 / VideoClock::dot_frequency;
    static constexpr double frame_ms = line_ms * VideoClock::lines_per_frame;
    static constexpr double pit_ms = CompatibilityClock::pit_period_ticks * 1000.0 / CompatibilityClock::pit_frequency;
    // The pinned PIC gates delivery by one PIT clock, truncated to nanoseconds.
    static constexpr double irq_delay_ms = (1000000000 / CompatibilityClock::pit_frequency) / 1000000.0;
    double period_ms = frame_ms, frame_target_ms = 0;
    unsigned line = 0;
    std::array<double,event_count> events{
        static_cast<float>(line_ms / 4), pit_ms,
        static_cast<float>(line_ms * visible_lines + vertical_irq_delay_ms),
        static_cast<float>(line_ms * VideoClock::retrace_line), static_cast<float>(line_ms * retrace_end_line),
        static_cast<float>(frame_ms), 1000
    };
    double dac_ms = std::numeric_limits<double>::infinity(), dac_period_ms = 0;
};

struct PlanarTransfer {
    std::uint64_t cycle;
    unsigned dwords, remaining;
};

struct PlanarCopy {
    std::vector<PlanarTransfer> transfers;
    std::array<std::uint64_t,5> port_cycles{};
    std::uint64_t request_cycle = 0;
};

struct CpuServiceWindow {
    std::uint64_t begin, end;
};

enum class IrqScheduling { external, timer_shell, device };

class RasterSchedule {
public:
    static constexpr std::uint64_t cycles_per_ms = 60000;

    explicit RasterSchedule(RasterPhase phase = {}, std::vector<CpuServiceWindow> services = {},
                            IrqScheduling interrupts = IrqScheduling::external,
                            std::function<std::uint64_t(std::uint64_t)> irq_work = {})
        : phase_(phase), services_(std::move(services)), irq_work_(std::move(irq_work)) {
        if (interrupts == IrqScheduling::timer_shell) { timer_.emplace(); }
        if ((interrupts == IrqScheduling::device) != static_cast<bool>(irq_work_)) {
            throw std::runtime_error("Device IRQ scheduling requires its service provider");
        }
        std::uint64_t end = 0;
        for (const auto& service : services_) {
            if (service.begin < end || service.end <= service.begin) { throw std::runtime_error("Invalid CPU service interval"); }
            end = service.end;
        }
    }

    PlanarCopy copy(std::uint64_t begin, unsigned page) {
        while (service_ < services_.size() && services_[service_].end <= begin) { ++service_; }
        PlanarCopy result;
        result.transfers.reserve(dwords_per_plane + RasterPhase::visible_lines);
        auto cycle = begin;
        auto limit = budget(cycle);
        while (cycle < service_busy_until_) {
            cycle = service_busy_until_;
            limit = budget(cycle);
        }
        in_copy_ = true;
        const auto instruction = [&](unsigned cost) {
            limit = dispatch(cycle,limit);
            cycle += cost;
            if (cycle >= limit) { ++cycle; }
        };
        for (unsigned i = 0; i < setup_instructions; ++i) { instruction(1); }
        for (unsigned plane = 0; plane < 4; ++plane) {
            if (plane != 0) { instruction(1); }
            limit = dispatch(cycle,limit);
            result.port_cycles[plane] = cycle + 1;
            instruction(out_cycles);
            for (unsigned i = 0; i < (plane == 0 ? 2u : 3u); ++i) { instruction(1); }
            unsigned remaining = dwords_per_plane;
            while (remaining != 0) {
                limit = dispatch(cycle,limit);
                // REP still transfers one dword when its opcode fetch uses
                // the final budget cycle. The normal-core exit costs one more.
                const auto available = std::max<std::uint64_t>(1,limit - cycle - 1);
                const auto count = static_cast<unsigned>(std::min<std::uint64_t>({remaining,dwords_per_dispatch,available}));
                result.transfers.push_back({cycle + 1 + count,count,remaining});
                instruction(1 + count);
                remaining -= count;
            }
        }
        for (unsigned i = 0; i < (page == 0 ? 4u : 3u); ++i) { instruction(1); }
        limit = dispatch(cycle,limit);
        result.port_cycles[4] = cycle + 1;
        result.request_cycle = cycle + out_cycles;
        instruction(out_cycles);
        in_copy_ = false;
        return result;
    }

    std::uint64_t idle(std::uint64_t begin, std::uint64_t minimum) {
        auto cycle = begin;
        auto limit = budget(cycle);
        while (cycle < service_busy_until_) {
            cycle = service_busy_until_;
            limit = budget(cycle);
        }
        in_copy_ = true;
        const auto deadline = begin + minimum;
        do {
            // CALLBACK_Idle executes twelve NOPs and its return callback.
            // The fixed-cycle backend then discards the remaining CPU budget.
            constexpr unsigned idle_instructions = 13;
            for (unsigned instruction = 0; instruction < idle_instructions; ++instruction) {
                limit = dispatch(cycle,limit);
                // The return callback exits the core directly, including when
                // it consumes the final budget unit.
                if (++cycle >= limit && instruction + 1 < idle_instructions) { ++cycle; }
            }
            cycle = std::max(cycle,limit);
        } while (cycle < deadline);
        in_copy_ = false;
        return cycle;
    }

    std::uint64_t cpu(std::uint64_t begin, std::uint64_t work) {
        auto cycle = begin;
        auto limit = budget(cycle);
        if (cycle < service_busy_until_) {
            cycle = service_busy_until_;
            limit = budget(cycle);
        }
        in_copy_ = true;
        while (work != 0) {
            limit = dispatch(cycle,limit);
            const auto consumed = std::min(work,limit - cycle);
            cycle += consumed;
            work -= consumed;
            if (cycle >= limit) { ++cycle; }
        }
        in_copy_ = false;
        return cycle;
    }

    // An unmasked, exhausted probe stops when its pending DAC event runs.
    void stop_dac_clock() { phase_.dac_period_ms = 0; }

    void io_completion(std::uint64_t cycle, std::function<void()> complete = {}) {
        io_completion_ms_ = static_cast<double>(cycle) / cycles_per_ms;
        io_complete_ = std::move(complete);
    }

    std::uint64_t instruction(std::uint64_t begin, unsigned work,
                              const std::function<void(std::uint64_t)>& effect) {
        auto cycle = begin;
        auto limit = budget(cycle);
        if (cycle < service_busy_until_) {
            cycle = service_busy_until_;
            limit = budget(cycle);
        }
        in_copy_ = true;
        limit = dispatch(cycle,limit);
        cycle += work;
        const auto previous_completion = io_completion_ms_;
        effect(cycle);
        // Adding an earlier PIC event forces the running normal core to exit.
        const auto rescheduled = io_completion_ms_ != previous_completion &&
            io_completion_ms_ * cycles_per_ms < static_cast<double>(limit);
        if (cycle >= limit || rescheduled) { ++cycle; }
        in_copy_ = false;
        return cycle;
    }

private:
    std::uint64_t dispatch(std::uint64_t& cycle, std::uint64_t limit) {
        if (cycle >= limit) { limit = budget(cycle); }
        // The shell runs on the same budgeted CPU timeline as the interrupted
        // REP. Timer phases advance across gaps between copies as well.
        while (pending_work_ != 0) {
            const auto work = std::exchange(pending_work_,0);
            for (std::uint64_t instruction = 0; instruction < work; ++instruction) {
                if (cycle >= limit) { limit = budget(cycle); }
                if (++cycle >= limit) { ++cycle; }
            }
        }
        if (service_ == services_.size() || services_[service_].begin > cycle) {
            return cycle >= limit ? budget(cycle) : limit;
        }
        while (service_ < services_.size() && services_[service_].begin <= cycle) {
            const auto& service = services_[service_++];
            cycle += service.end - service.begin;
        }
        return budget(cycle);
    }

    std::uint64_t budget(std::uint64_t cycle) {
        auto next = std::min_element(phase_.events.begin(),phase_.events.end());
        while (std::min({*next,irq_check_ms_,phase_.dac_ms,io_completion_ms_}) * cycles_per_ms <= static_cast<double>(cycle)) {
            if (io_completion_ms_ < std::min({*next,irq_check_ms_,phase_.dac_ms})) {
                io_completion_ms_ = std::numeric_limits<double>::infinity();
                if (io_complete_) { std::exchange(io_complete_,{})(); }
                continue;
            }
            if (phase_.dac_ms < std::min(*next,irq_check_ms_)) {
                phase_.dac_ms = phase_.dac_period_ms == 0 ? std::numeric_limits<double>::infinity() :
                    phase_.dac_ms + phase_.dac_period_ms;
                continue;
            }
            if (irq_check_ms_ < *next) {
                const auto service_cycle = static_cast<std::uint64_t>(irq_check_ms_ * cycles_per_ms);
                irq_check_ms_ = std::numeric_limits<double>::infinity();
                ++irq_number_;
                if (timer_) {
                    const auto work = timer_->interrupt();
                    if (in_copy_) { pending_work_ += work; }
                }
                if (irq_work_) {
                    const auto work = irq_work_(irq_number_);
                    if (in_copy_) { pending_work_ += work; }
                    else { service_busy_until_ = std::max(service_busy_until_,service_cycle) + work; }
                }
                continue;
            }
            const auto event = static_cast<unsigned>(next - phase_.events.begin());
            const auto time = *next;
            *next = std::numeric_limits<double>::infinity();
            if (event == RasterPhase::scanline) {
                if (++phase_.line < RasterPhase::visible_lines) { *next = time + static_cast<float>(RasterPhase::line_ms); }
            } else if (event == RasterPhase::pit) {
                *next = time + RasterPhase::pit_ms;
                irq_check_ms_ = time + RasterPhase::irq_delay_ms;
            } else if (event == RasterPhase::frame) {
                phase_.frame_target_ms += phase_.period_ms;
                const auto adjustment = std::clamp((phase_.frame_target_ms - time) * .25,-.1,.1);
                *next = time + static_cast<float>(phase_.period_ms) + adjustment;
                phase_.line = 0;
                phase_.events[RasterPhase::scanline] = time + static_cast<float>(RasterPhase::line_ms / 4);
                phase_.events[RasterPhase::vertical_irq] = time + static_cast<float>(RasterPhase::line_ms * RasterPhase::visible_lines + RasterPhase::vertical_irq_delay_ms);
                phase_.events[RasterPhase::display_latch] = time + static_cast<float>(RasterPhase::line_ms * VideoClock::retrace_line);
                phase_.events[RasterPhase::panning] = time + static_cast<float>(RasterPhase::line_ms * RasterPhase::retrace_end_line);
            } else if (event == RasterPhase::rtc) {
                *next = time + 1000;
            }
            next = std::min_element(phase_.events.begin(),phase_.events.end());
        }
        const auto millisecond = (cycle / cycles_per_ms + 1) * cycles_per_ms;
        const auto event_cycle = static_cast<std::uint64_t>(std::min(static_cast<double>(millisecond),
            std::min({*next,irq_check_ms_,phase_.dac_ms,io_completion_ms_}) * cycles_per_ms));
        return std::max(cycle + 1,event_cycle);
    }

    static constexpr unsigned dwords_per_plane = 8000;
    static constexpr unsigned dwords_per_dispatch = 4, setup_instructions = 14;
    // ISA word delay is 660 ns. The normal core charges 3/4 of that delay.
    static constexpr unsigned out_cycles = 1 + (cycles_per_ms * 660 * 3) / 4000000;
    RasterPhase phase_;
    std::vector<CpuServiceWindow> services_;
    std::size_t service_ = 0;
    double irq_check_ms_ = std::numeric_limits<double>::infinity();
    double io_completion_ms_ = std::numeric_limits<double>::infinity();
    std::function<void()> io_complete_;
    std::optional<TimerService> timer_;
    std::uint64_t pending_work_ = 0;
    std::function<std::uint64_t(std::uint64_t)> irq_work_;
    std::uint64_t irq_number_ = 0, service_busy_until_ = 0;
    bool in_copy_ = false;
};

}
