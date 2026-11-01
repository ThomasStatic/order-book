#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "clock.hpp"

// What the machine itself costs a thread that does nothing but watch the clock. Measured
// before and after a session, on the same core and at the same priority as the measured
// thread, so a percentile the machine could explain is marked and never blamed on the engine.
namespace order_book::latency {

inline constexpr std::array<double, 3> noiseThresholdsMicroseconds{1.0, 10.0, 100.0};

struct NoiseFloor {
    double seconds = 0;
    // Gaps between successive readings over each threshold.
    std::array<std::uint64_t, noiseThresholdsMicroseconds.size()> gapsOver{};
    double maximumNanoseconds = 0;
    // The share of the time spent inside gaps over the lowest threshold.
    double lostShare = 0;

    double gapsPerSecond(std::size_t threshold) const {
        return seconds > 0 ? static_cast<double>(gapsOver[threshold]) / seconds : 0.0;
    }
};

// Watches the clock until duration ticks have passed.
template <typename DriverClock>
NoiseFloor measureNoiseFloor(DriverClock& clock, Ticks duration, double ticksPerNanosecond) {
    std::array<Ticks, noiseThresholdsMicroseconds.size()> thresholds{};
    for (std::size_t i = 0; i < thresholds.size(); ++i) {
        thresholds[i] = static_cast<Ticks>(noiseThresholdsMicroseconds[i] * 1000.0 * ticksPerNanosecond);
    }

    NoiseFloor floor;
    Ticks lost = 0;
    Ticks worst = 0;
    const Ticks start = clock.now();
    Ticks previous = start;
    while (previous - start < duration) {
        const Ticks next = clock.now();
        const Ticks gap = next - previous;
        previous = next;
        if (gap > thresholds[0]) {
            lost += gap;
            worst = gap > worst ? gap : worst;
            for (std::size_t i = 0; i < thresholds.size(); ++i) {
                floor.gapsOver[i] += gap > thresholds[i] ? 1U : 0U;
            }
        }
    }
    const Ticks elapsed = previous - start;
    floor.seconds = static_cast<double>(elapsed) / ticksPerNanosecond / 1e9;
    floor.maximumNanoseconds = static_cast<double>(worst) / ticksPerNanosecond;
    floor.lostShare = elapsed > 0 ? static_cast<double>(lost) / static_cast<double>(elapsed) : 0.0;
    return floor;
}

}  // namespace order_book::latency
