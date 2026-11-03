#include "timetable.hpp"

#include <algorithm>
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

ArrivalTally::ArrivalTally(const std::vector<Ticks>& schedule, Ticks start, Ticks discard)
    : schedule_(&schedule), start_(start), discard_(discard), firstSeen_(schedule.size(), false) {}

void ArrivalTally::arrived(std::size_t index, Ticks at, bool last) {
    const std::vector<Ticks>& schedule = *schedule_;
    const Ticks scheduled = start_ + schedule[index];
    const Ticks taken = at > scheduled ? at - scheduled : 0;  // cores may disagree by a few ns
    const bool recorded = schedule[index] >= discard_;
    if (!firstSeen_[index]) {
        firstSeen_[index] = true;
        if (recorded) {
            first_.record(taken);
        }
    }
    if (!last) {
        return;
    }
    ++answered_;
    lastArrival_ = std::max(lastArrival_, at);
    if (recorded) {
        response_.record(taken);
    }
    const std::size_t commands = schedule.size();
    if (index >= commands - commands / 5) {
        Ticks& least = leastResponse_[index >= commands - commands / 10 ? 1 : 0];
        least = std::min(least, taken);
    }
}

bool ArrivalTally::sustained() const {
    const std::vector<Ticks>& schedule = *schedule_;
    if (schedule.size() < 2) {
        return true;
    }
    const Ticks meanGap = schedule.back() / (schedule.size() - 1);
    return !backlogGrowing(leastResponse_[0], leastResponse_[1], meanGap);
}

}  // namespace order_book::latency
