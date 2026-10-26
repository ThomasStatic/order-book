#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "workload.hpp"

// Runs generated workloads against the book and times them. Only the timed commands are
// inside the timed region: generating the workload, building the book, preloading it,
// rendering CSV text, checking the outcome, and destroying the book all happen outside it.
namespace order_book::benchmark {

// CORE times the engine alone: each command goes straight to OrderBook::submitOrder or
// cancelOrder, and no CSV is involved anywhere. REPLAY times the end-to-end path of
// docs/replay-format.md from text already in memory: parsing, the replay engine's dispatch
// and event collection, the final snapshot, and the formatted summary. Disk I/O is left out
// of both, since it would mostly measure the file cache.
enum class BenchmarkKind { CORE, REPLAY };

const char* benchmarkKindName(BenchmarkKind kind);
// Throws std::invalid_argument on a name other than core or replay.
BenchmarkKind parseBenchmarkKind(std::string_view name);

// One timed run of a workload's timed commands on a freshly preloaded book.
struct Measurement {
    std::string workload;
    BenchmarkKind kind = BenchmarkKind::CORE;
    // Counted from 1, separately for warm-ups and measured repetitions.
    std::size_t repetition = 0;
    // A warm-up's time is kept with the rest but left out of every summary.
    bool warmup = false;
    std::size_t commands = 0;
    std::int64_t elapsedNanoseconds = 0;
    // What the book did, counted from its results; it always equals the workload's expected
    // outcome, or the run throws instead of returning a measurement.
    WorkloadOutcome outcome;

    double operationsPerSecond() const;
    double nanosecondsPerCommand() const;
};

// commands / elapsed seconds; 0 when nothing was timed.
double operationsPerSecond(std::size_t commands, std::int64_t elapsedNanoseconds);

struct RunSettings {
    std::size_t warmups = 1;
    std::size_t repetitions = 5;
};

// Times the workload's commands once on a fresh book, preloaded outside the timed region.
// Throws std::runtime_error, describing both, if the book's outcome differs from the one the
// workload's model predicted, so a measurement never stands for work the book did not do.
Measurement runOnce(const Workload& workload, BenchmarkKind kind);

// settings.warmups runs, then settings.repetitions measured runs, each on its own fresh book,
// in that order. Every run is returned, warm-ups first.
std::vector<Measurement> runRepeated(const Workload& workload, BenchmarkKind kind, const RunSettings& settings);

// The workload's timed commands as a replay file (header, then one record per command,
// numbered from 1), for the REPLAY kind and for inspecting a workload by hand.
std::string renderReplayCsv(const std::vector<BenchmarkCommand>& commands);

}  // namespace order_book::benchmark
