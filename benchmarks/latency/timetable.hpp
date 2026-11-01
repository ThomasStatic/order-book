#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "clock.hpp"
#include "histogram.hpp"

// The load drivers. Closed-loop sends each command when the last returns, which is right only
// for service time and capacity: a stall then delays the commands behind it, so they are never
// measured, and the stall all but vanishes (coordinated omission). Every other claim runs on a
// timetable: each command has a time to go out, it is timed from that time, and a late one goes
// straight out with nothing skipped.
//
// Clock needs now() and spinUntil(deadline). A target is called as target(index, clock), runs
// one command, and returns the readings taken just before and just after it, so whatever it does
// to get ready (loading a command, building an order) and to tidy up (counting a result,
// destroying it) is outside the interval.
namespace order_book::latency {

// The readings either side of one command.
struct Stamps {
    Ticks started;
    Ticks done;
};

struct TimetableRun {
    Histogram service;   // from just before the command to the return
    Histogram response;  // from the scheduled send to the return
    Histogram lateness;  // from the scheduled send to the real send, so a stalled driver shows
    Ticks elapsed = 0;   // from the start to the last return
    // False if the backlog was still growing at the end, when no percentile may be quoted.
    bool sustained = true;
};

// A backlog that drains leaves a send on time now and then, however bad the bursts, so the
// least lateness in a tenth of the run says how far behind the timetable it has fallen for good.
// The run is not sustained if that rose by more than one mean gap across the final two tenths.
constexpr bool backlogGrowing(Ticks previousTenth, Ticks finalTenth, Ticks meanGap) {
    return previousTenth != std::numeric_limits<Ticks>::max() && finalTenth > previousTenth + meanGap;
}

// schedule[i] is command i's send time, in ticks from the run's start.
template <typename DriverClock, typename Target>
TimetableRun runTimetable(const std::vector<Ticks>& schedule, DriverClock& clock, Target& target) {
    TimetableRun run;
    const std::size_t commands = schedule.size();
    const std::size_t previousTenthStart = commands - commands / 5;
    const std::size_t finalTenthStart = commands - commands / 10;
    Ticks leastLateness[2] = {std::numeric_limits<Ticks>::max(), std::numeric_limits<Ticks>::max()};

    const Ticks start = clock.now();
    Ticks done = start;
    for (std::size_t index = 0; index < commands; ++index) {
        const Ticks scheduled = start + schedule[index];
        const Ticks sent = clock.spinUntil(scheduled);
        const Stamps stamps = target(index, clock);
        done = stamps.done;
        const Ticks lateness = sent - scheduled;
        run.lateness.record(lateness);
        run.service.record(stamps.done - stamps.started);
        run.response.record(stamps.done - scheduled);
        if (index >= previousTenthStart) {
            Ticks& least = leastLateness[index >= finalTenthStart ? 1 : 0];
            least = std::min(least, lateness);
        }
    }
    run.elapsed = done - start;
    if (commands > 1) {
        const Ticks meanGap = schedule.back() / (commands - 1);
        run.sustained = !backlogGrowing(leastLateness[0], leastLateness[1], meanGap);
    }
    return run;
}

// Each command goes out when the last returns. Service time only.
template <typename DriverClock, typename Target>
Histogram runClosedLoop(std::size_t commands, DriverClock& clock, Target& target) {
    Histogram service;
    for (std::size_t index = 0; index < commands; ++index) {
        const Stamps stamps = target(index, clock);
        service.record(stamps.done - stamps.started);
    }
    return service;
}

// count send times gapTicks apart, rounded down from the exact time so nothing accumulates.
std::vector<Ticks> fixedRateSchedule(std::size_t count, double gapTicks);

// count send times of a Poisson process with this mean gap, from the seed alone. A fixed
// timetable hides the queues that bursts build.
std::vector<Ticks> poissonSchedule(std::size_t count, double meanGapTicks, std::uint64_t seed);

}  // namespace order_book::latency
