#include "clock.hpp"

#include <array>
#include <numeric>

#include "histogram.hpp"

#if ORDER_BOOK_LATENCY_HAS_TSC && !defined(_MSC_VER)
#include <cpuid.h>
#endif

namespace order_book::latency {

namespace {

bool hasInvariantTsc() {
#if ORDER_BOOK_LATENCY_HAS_TSC
    constexpr unsigned int advancedPowerLeaf = 0x80000007U;
    constexpr unsigned int invariantTscBit = 1U << 8;
#if defined(_MSC_VER)
    std::array<int, 4> registers{};
    __cpuid(registers.data(), static_cast<int>(0x80000000U));
    if (static_cast<unsigned int>(registers[0]) < advancedPowerLeaf) {
        return false;
    }
    __cpuid(registers.data(), static_cast<int>(advancedPowerLeaf));
    return (static_cast<unsigned int>(registers[3]) & invariantTscBit) != 0;
#else
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (__get_cpuid(advancedPowerLeaf, &eax, &ebx, &ecx, &edx) == 0) {
        return false;
    }
    return (edx & invariantTscBit) != 0;
#endif
#else
    return false;
#endif
}

double calibrate(const Clock& clock) {
    using Steady = std::chrono::steady_clock;
    const Steady::time_point steadyStart = Steady::now();
    const Ticks ticksStart = clock.now();
    while (Steady::now() - steadyStart < std::chrono::milliseconds(50)) {
    }
    const Ticks ticksStop = clock.now();
    const Steady::time_point steadyStop = Steady::now();
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(steadyStop - steadyStart).count();
    return static_cast<double>(ticksStop - ticksStart) / static_cast<double>(nanoseconds);
}

// The greatest common divisor of successive readings' differences: the clock's real step,
// which is not one tick on every CPU (Zen 3's TSC advances 39 ticks at a time).
Ticks measureStep(const Clock& clock) {
    Ticks step = 0;
    Ticks previous = clock.now();
    for (int i = 0; i < 100000; ++i) {
        const Ticks next = clock.now();
        step = std::gcd(step, next - previous);
        previous = next;
    }
    return step;
}

double measureReadCost(const Clock& clock, double ticksPerNanosecond) {
    constexpr int reads = 1000000;
    const Ticks start = clock.now();
    Ticks last = start;
    for (int i = 0; i < reads; ++i) {
        last = clock.now();
    }
    return static_cast<double>(last - start) / ticksPerNanosecond / static_cast<double>(reads);
}

}  // namespace

Clock detectClock() {
    return Clock(hasInvariantTsc());
}

ClockRecord measureClock(const Clock& clock) {
    ClockRecord record;
    record.name = clock.usesTsc() ? std::string(tscClockName) : std::string("steady_clock");
    record.ticksPerNanosecond = calibrate(clock);
    record.stepNanoseconds = static_cast<double>(measureStep(clock)) / record.ticksPerNanosecond;
    record.readCostNanoseconds = measureReadCost(clock, record.ticksPerNanosecond);

    Histogram emptyPairs;
    for (int i = 0; i < 1000000; ++i) {
        const Ticks start = clock.now();
        const Ticks stop = clock.now();
        emptyPairs.record(stop - start);
    }
    record.emptyPairP50Nanoseconds =
        static_cast<double>(emptyPairs.valueAt(Percentile{50000, "p50"})) / record.ticksPerNanosecond;
    record.emptyPairP99Nanoseconds =
        static_cast<double>(emptyPairs.valueAt(Percentile{99000, "p99"})) / record.ticksPerNanosecond;
    return record;
}

}  // namespace order_book::latency
