#pragma once

#include "clock.hpp"
#include <stdexcept>

namespace swiv3d::detail {

// Original three-timer profile for DPMI/timer/message/BIOS shell work.
// Digital callback execution remains outside this profile.
class TimerService {
public:
    explicit TimerService(unsigned message_us = 0, unsigned bios_us = 0)
        : message_us_(message_us), bios_us_(bios_us) {
        if (message_us >= message_period_us || bios_us >= bios_period_us) {
            throw std::runtime_error("Invalid timer service phase");
        }
    }

    std::uint64_t interrupt() {
        // Charges include decoded scalar/REP instructions and ISA port delays
        // for the pinned DOS/4GW entry, API_timer and ordinary BIOS paths.
        constexpr unsigned dpmi_entry = 339, dispatch = 279, dpmi_return = 80;
        constexpr unsigned message_post = 71, bios_chain = 706;
        auto work = std::uint64_t(dpmi_entry + dispatch + dpmi_return);
        message_us_ += CompatibilityClock::driver_period_us;
        if (message_us_ >= message_period_us) { message_us_ -= message_period_us; work += message_post; }
        bios_us_ += CompatibilityClock::driver_period_us;
        if (bios_us_ >= bios_period_us) { bios_us_ -= bios_period_us; work += bios_chain; }
        return work;
    }

private:
    static constexpr unsigned message_period_us = 25000, bios_period_us = 54925;
    unsigned message_us_, bios_us_;
};

}
