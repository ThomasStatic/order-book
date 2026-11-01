#include "timetable.hpp"

#include <cmath>
#include <random>

namespace order_book::latency {

std::vector<Ticks> fixedRateSchedule(std::size_t count, double gapTicks) {
    std::vector<Ticks> schedule(count);
    for (std::size_t i = 0; i < count; ++i) {
        schedule[i] = static_cast<Ticks>(static_cast<double>(i) * gapTicks);
    }
    return schedule;
}

std::vector<Ticks> poissonSchedule(std::size_t count, double meanGapTicks, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    std::vector<Ticks> schedule(count);
    double time = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        schedule[i] = static_cast<Ticks>(time);
        // A raw draw, never a standard distribution, as in the workload generators.
        const double uniform = (static_cast<double>(random() >> 11) + 0.5) * 0x1.0p-53;
        time -= std::log(uniform) * meanGapTicks;
    }
    return schedule;
}

}  // namespace order_book::latency
