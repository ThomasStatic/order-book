#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "benchmark_runner.hpp"
#include "clock.hpp"
#include "thread_setup.hpp"
#include "timetable.hpp"
#include "workload.hpp"

// The hand-off interval: from just before a command goes into the command ring until its last
// event comes back out of the response ring, on the sending thread, through a real matching
// thread and a real book. Under load the two rings are most of it, and on an idle engine a
// thread's wake-up is, so a report keeps those rows apart.
namespace order_book::latency {

class HandOffTarget {
public:
    // Starts the matching thread, which runs setUpMatchingThread, then builds and preloads the
    // book, untimed, and waits as the engine's does, spinning for spinWindow before it sleeps.
    // The workload must outlive the target.
    HandOffTarget(const benchmark::Workload& workload, std::function<void()> setUpMatchingThread,
                  std::chrono::nanoseconds spinWindow = runtime::defaultSpinWindow);

    // The matching thread's setup holds this target's address.
    HandOffTarget(const HandOffTarget&) = delete;
    HandOffTarget& operator=(const HandOffTarget&) = delete;

    template <typename DriverClock>
    Stamps operator()(std::size_t index, DriverClock& clock) {
        run_.prepare(index);
        const Ticks started = clock.now();
        run_.sendPrepared();
        run_.awaitResponse();
        const Ticks done = clock.now();
        return {started, done};
    }

    // Stops the matching thread. Throws std::runtime_error, describing both, if the book's outcome
    // differs from the workload's prediction.
    void checkOutcome();

    std::uint64_t capacityGrowths() const { return 0; }  // checkOutcome refuses any growth

    // The matching thread's CPU so far, as ThreadCpuTime reads it. Before checkOutcome.
    std::uint64_t matchingCpu() const { return meter_->reading(); }

private:
    std::vector<benchmark::Workload> workloads_;
    std::optional<ThreadCpuTime> meter_;  // made on the matching thread, before the books
    benchmark::QueueRun run_;
};

}  // namespace order_book::latency
