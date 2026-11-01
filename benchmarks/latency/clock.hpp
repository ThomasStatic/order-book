#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#if defined(_M_X64) || defined(__x86_64__)
#define ORDER_BOOK_LATENCY_HAS_TSC 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#else
#define ORDER_BOOK_LATENCY_HAS_TSC 0
#endif

// The one clock every latency interval is stamped with: rdtscp;lfence where the CPU reports an
// invariant TSC, and steady_clock otherwise, which includes Arm. Its real step and its cost are
// measured, never assumed, and recorded with every result. Nothing is ever subtracted from a
// reading: a percentile minus a constant is not the percentile of the difference.
namespace order_book::latency {

using Ticks = std::uint64_t;

// The clock's name in a result: only a result from this one can be quoted.
inline constexpr std::string_view tscClockName = "rdtscp;lfence";

class Clock {
public:
    explicit Clock(bool useTsc) : useTsc_(useTsc) {}

    Ticks now() const {
#if ORDER_BOOK_LATENCY_HAS_TSC
        if (useTsc_) {
            unsigned int processor = 0;
            const Ticks ticks = __rdtscp(&processor);
            _mm_lfence();
            return ticks;
        }
#endif
        return static_cast<Ticks>(std::chrono::steady_clock::now().time_since_epoch().count());
    }

    // Returns the first reading at or past the deadline, at once for one already passed.
    Ticks spinUntil(Ticks deadline) const {
        Ticks ticks = now();
        while (ticks < deadline) {
            ticks = now();
        }
        return ticks;
    }

    bool usesTsc() const { return useTsc_; }

private:
    bool useTsc_;
};

// The TSC if this CPU reports it invariant, otherwise steady_clock.
Clock detectClock();

struct ClockRecord {
    std::string name;
    double ticksPerNanosecond = 0;
    double stepNanoseconds = 0;
    double readCostNanoseconds = 0;
    double emptyPairP50Nanoseconds = 0;
    double emptyPairP99Nanoseconds = 0;
};

// Calibrates the clock against steady_clock and measures its step and cost, over about 100 ms.
ClockRecord measureClock(const Clock& clock);

}  // namespace order_book::latency
