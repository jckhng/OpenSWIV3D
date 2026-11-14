#pragma once

#include "clock.hpp"

#include <cstdint>
#include <optional>

namespace swiv3d::detail {

// SB16 status polling in the pinned compatibility profile. Consecutive reads
// advance sixteen bus slots, with a time-based resync after twenty microseconds.
class DspBusyClock {
public:
    bool busy(std::uint64_t cycle) {
        constexpr unsigned busy_rate = 8333333 / 6 / 16;
        constexpr unsigned slots = 16, busy_slots = 8;
        constexpr auto resync_cycles = CompatibilityClock::cpu_frequency / 50000;
        if (!last_read_ || cycle - *last_read_ >= resync_cycles) {
            slot_ = static_cast<unsigned>((cycle % CompatibilityClock::cpu_frequency * busy_rate %
                CompatibilityClock::cpu_frequency) * slots / CompatibilityClock::cpu_frequency);
        } else { slot_ = (slot_ + 1) % slots; }
        last_read_ = cycle;
        return slot_ < busy_slots || write_busy_;
    }

    std::uint64_t write(std::uint64_t cycle) {
        constexpr auto write_cycles = CompatibilityClock::cpu_frequency / 1000000;
        write_busy_ = true;
        return cycle + write_cycles;
    }

    void complete() { write_busy_ = false; }

private:
    std::optional<std::uint64_t> last_read_;
    bool write_busy_ = false;
    unsigned slot_ = 0;
};

}
