#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
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
// book. reserve-capacity: reserve(BookCapacity) on a new book, with a quarter as many stops and
// a tenth as many levels per side. new-level-near: a resting order on a new level just behind
// the touch. new-level-far: one behind every other level. Only those operations are timed:
// building a structure and cancelling what a pass added are not.
std::vector<ComponentResult> runCapacityGroup(const CapacitySettings& settings);

// The log group, on a workload's real commands, since cancels and replaces encode differently:
// log-encode, an encoded and checksummed record into memory; log-read, reading and verifying a
// whole log in memory; and csv-read, ReplayReader on the same commands as v3 CSV text.
// The entry group: symbol-compare, one comparison of two symbols; execute-command, a command
// through executeCommand on a book, overwriting one reused result.
struct WorkloadComponentSettings {
    std::size_t commands = 1000000;
    std::size_t passes = 5;
    std::uint64_t seed = 42;
    std::vector<std::string> workloads{"matching-heavy-request", "quote-refresh", "pending-stops"};
};
std::vector<ComponentResult> runLogGroup(const WorkloadComponentSettings& settings);
std::vector<ComponentResult> runEntryGroup(const WorkloadComponentSettings& settings);
std::vector<ComponentResult> runWireGroup(const WorkloadComponentSettings& settings);

// The images group: a book image's costs, by book size and by how scattered the book's nodes
// are, since the cost is chasing them, not moving bytes. Books take a workload's shape, its
// initial orders set to each size: deep-book's at each of deepBookOrders, over its 2000 levels
// or half as many levels as orders if fewer, and mostly-resting's at mostlyRestingOrders. Each
// in three states: restored (rebuilt from its own image, so compact, which is what a worker
// starts from), aged (after agingCommands of the workload's own timed commands, so its size
// moves as the workload's does), and shuffled (its preload rested in a random order, the worst
// case). Operations: write-image, restore, check-invariants, and image-proof (restore, then
// writeImage again and compare the bytes, the proof a checkpoint must pass), each one per
// pass, with the caches evicted before each pass by walking evictionBytes.
struct ImageSettings {
    std::vector<std::size_t> deepBookOrders{1000, 10000, 100000, 1000000};
    std::size_t mostlyRestingOrders = 1000000;
    std::size_t agingCommands = 1000000;
    std::size_t passes = 5;
    std::size_t evictionBytes = 64 * 1024 * 1024;
    std::uint64_t seed = 42;
};
std::vector<ComponentResult> runImageGroup(const ImageSettings& settings);

// The durability group: what checkpoints cost the log's drive and a worker, in a fresh directory
// under logParent, which should be on the log's drive. publish-checkpoint: one checkpoint
// published through FileLogStorage (written to .tmp, flushed, read back, renamed, which flushes
// the directory), by image size: deep-book's preloaded book at each of deepBookOrders.
// roll-pass: one roll-forward pass over a real closed log file of fileRecords commands, which
// instruments books of workload share, each with a stream of its own written in turn by a real
// LogWriter: every book restored from its empty checkpoint, replayed, proved, and published at
// the file's end, so ns_per_op is per command. recover: a start's survey and one shard's
// recovery, each book restored from its checkpoint at that file's end and a second file of as
// many commands replayed, so ns_per_op is per command replayed.
struct DurabilitySettings {
    std::vector<std::size_t> deepBookOrders{1000, 10000, 100000, 1000000};
    std::size_t publishPasses = 5;
    std::size_t fileRecords = 0;  // 0: as many as a default 256 MiB log file holds, 6.7M
    std::size_t instruments = 8;
    std::string workload = "matching-heavy-request";
    std::size_t rollPasses = 3;
    std::uint64_t seed = 42;
    std::filesystem::path logParent;  // empty: the system's temporary directory
};
std::vector<ComponentResult> runDurabilityGroup(const DurabilitySettings& settings);

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
