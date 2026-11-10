#include "det_clock.h"
#include "timebase_contract.h"
#include <cassert>
#include <chrono>
#include <iostream>

int main() {
    using Clock = std::chrono::steady_clock;
    DetClock::Enable();
    DetClock::ResetPacing();
    DetClock::PaceTo(DetClock::Now());
    const auto start = Clock::now();
    for (int pass = 0; pass < 7; ++pass) {
        DetClock::SetRollbackResimulating(true);
        const auto next = DetClock::Now() + TimeBaseContract::kTicksPerSecond;
        DetClock::PaceTo(next);
        DetClock::AdvanceTo(next);
        assert(DetClock::Now() == next);
        DetClock::SetRollbackResimulating(false);
    }
    // Returning to forward play must not repay seven seconds of replayed time.
    DetClock::PaceTo(DetClock::Now());
    assert(Clock::now() - start < std::chrono::milliseconds(200));
    const auto before = DetClock::Now();
    DetClock::PaceTo(before + TimeBaseContract::kTicksPerSecond / 100);
    assert(DetClock::Now() == before); // Pacing itself cannot change guest time.
    std::cout << "Rollback clock: virtual events advance without replay sleeps or deferred pacing debt\n";
}
