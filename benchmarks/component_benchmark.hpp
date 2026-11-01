#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Component costs: one operation on a prepared structure, in bulk, for what no workload shows.
// A result is a mean cost per operation, and it is never a percentile: every percentile comes
// from the latency harness.
namespace order_book::benchmark {

struct ComponentResult {
    std::string operation;
    std::string workload;  // what the operation ran on
    std::string state;     // the structure's condition, or which thread ran it
    std::size_t orders = 0;
    std::size_t levels = 0;
    std::size_t passes = 0;
    std::int64_t elapsedNanoseconds = 0;
    std::size_t operations = 0;  // across every pass

    double nanosecondsPerOperation() const;
};

// One header line, then a line per result: operation, workload, state, orders, levels, passes,
// elapsed_ns, ns_per_op.
std::string formatComponentCsv(const std::vector<ComponentResult>& results);

// The capacity group: what a book pays to grow.
struct CapacitySettings {
    // Each entry is exactly three quarters of a power of two, so the next order grows the table.
    std::vector<std::size_t> orderCounts{12288, 98304, 786432};
    std::vector<std::size_t> levelCounts{2000, 20000, 200000};  // per side
    std::size_t growthPasses = 5;
    std::size_t levelPasses = 100;
};

// grow-to: the one emplace that grows a full index. reserve-orders: OrderBook::reserve on a new
// book. new-level-near: a resting order on a new level just behind the touch. new-level-far: one
// behind every other level. Only those operations are timed: building a structure and cancelling
// what a pass added are not.
std::vector<ComponentResult> runCapacityGroup(const CapacitySettings& settings);

// The controls that find the cause of a fall in scaling, each on the same threads as the run it
// explains. If the CPU loop falls too, the cause is frequency. If only the walks fall, it is
// the shared cache or memory.
struct ControlSettings {
    std::size_t threads = 1;
    // Fits in L2, fits in the shared L3, and exceeds it.
    std::vector<std::size_t> walkBytes{256 * 1024, 8 * 1024 * 1024, 256 * 1024 * 1024};
    std::size_t cpuIterations = 200000000;
    std::size_t walkSteps = 20000000;
    // Called first on each thread with its index, to pin and raise it.
    std::function<void(std::size_t)> setUpThread;
};

// cpu-loop touches no memory. memory-walk chases a random cycle through one cache line at a
// time, so each step is one load the prefetcher can't predict. Every thread starts together
// and each gives one result, per operation and size.
std::vector<ComponentResult> runControlGroup(const ControlSettings& settings);

}  // namespace order_book::benchmark
