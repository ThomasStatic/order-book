#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "benchmark_runner.hpp"
#include "scaling_run.hpp"
#include "workload.hpp"

// The threaded kinds at N shards and M threads, on real threads: every run does what its books
// predict, a book that doesn't fails it, and each records its setup. No test asserts a speed.
// Kept small, two small-book books a shard, so the sanitizer builds run it in time; spinning
// threads past a runner's cores would make those crawl, so no case spins without end.

namespace {

using namespace order_book::benchmark;
using order_book::reference::Spread;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename Function>
void expectThrows(Function function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    expect(false, message);
}

struct Cell {
    BenchmarkKind kind;
    std::size_t shards;
    std::size_t threads;
    Spread spread;
    Placement placement;
    bool tamper;  // one cell of each kind also runs with a wrong prediction
};

std::string describe(const Cell& cell) {
    return std::string(benchmarkKindName(cell.kind)) + " at (" + std::to_string(cell.shards) + ", " + std::to_string(cell.threads) + "), " +
           spreadName(cell.spread) + ", " + placementName(cell.placement);
}

void testEveryCellDoesWhatItsBooksPredict() {
    const std::vector<Cell> cells{
        {BenchmarkKind::SHARD, 1, 1, Spread::UNIFORM, Placement::EVEN, false},
        {BenchmarkKind::SHARD, 3, 1, Spread::UNIFORM, Placement::EVEN, true},
        {BenchmarkKind::QUEUE, 2, 2, Spread::UNIFORM, Placement::EVEN, true},
        {BenchmarkKind::QUEUE, 3, 1, Spread::SKEWED, Placement::HASH, false},
        {BenchmarkKind::GATEWAY, 2, 1, Spread::UNIFORM, Placement::EVEN, false},
        {BenchmarkKind::GATEWAY, 4, 2, Spread::SKEWED, Placement::HOT_ALONE, true},
    };
    for (const Cell& cell : cells) {
        // Hash placement needs enough books that no shard is left empty.
        const std::size_t instruments = cell.placement == Placement::HASH ? 16 : 2 * cell.shards;
        const std::vector<Workload> workloads =
            spreadWorkloads(findWorkload("small-book"), 500 * cell.shards, 42, instruments, cell.spread);
        ScalingSettings settings;
        settings.shards = cell.shards;
        settings.threads = cell.threads;
        settings.spread = cell.spread;
        settings.placement = cell.placement;
        settings.spinWindow = std::chrono::nanoseconds::zero();  // a lost wake-up hangs, and fails under the timeout
        const Measurement run = runScaledOnce(workloads, cell.kind, settings);
        expect(run.outcome == totalOutcome(workloads) && run.commands == 500 * cell.shards, describe(cell) + ": every book's prediction");
        expect(run.shards == cell.shards && run.instruments == instruments && run.spread == spreadName(cell.spread) &&
                   run.placement == placementName(cell.placement) && !run.pinned && run.layout == "unpinned",
               describe(cell) + ": it records its setup");
        expect(run.partRates.size() == cell.shards, describe(cell) + ": a rate per shard");
        expect(run.elapsedNanoseconds > 0 && run.fingerprint == runFingerprint(workloads, cell.spread, cell.shards, cell.placement),
               describe(cell) + ": timed and fingerprinted");
        expect(cell.kind != BenchmarkKind::QUEUE || run.producers == cell.threads, describe(cell) + ": its producers");
        expect(cell.kind != BenchmarkKind::GATEWAY || (run.gatewayThreads == cell.threads && run.connections == instruments),
               describe(cell) + ": its gateway threads and connections");

        if (cell.tamper) {
            std::vector<Workload> tampered = workloads;
            tampered.back().expected.trades += 1;
            expectThrows([&] { static_cast<void>(runScaledOnce(tampered, cell.kind, settings)); }, describe(cell) + ": a wrong prediction fails it");
        }
    }
}

// A log on every shard, and the frame budget lifted.
void testLoggedAndUnboundedRuns() {
    const std::filesystem::path parent = std::filesystem::temp_directory_path() / "order_book_benchmark_scaling_tests";
    std::filesystem::remove_all(parent);
    std::filesystem::create_directories(parent);
    const std::vector<Workload> workloads = spreadWorkloads(findWorkload("small-book"), 2000, 7, 4, Spread::UNIFORM);
    for (const BenchmarkKind kind : {BenchmarkKind::QUEUE, BenchmarkKind::GATEWAY}) {
        ScalingSettings settings;
        settings.shards = 2;
        settings.durability = RunDurability::PROCESS;
        settings.logParent = parent;
        settings.framesPerPass = std::numeric_limits<std::size_t>::max();
        const Measurement run = runScaledOnce(workloads, kind, settings);
        expect(run.outcome == totalOutcome(workloads) && run.durability == RunDurability::PROCESS && run.rollPasses == 0,
               "a logged run at N = 2, with no worker");
    }
    // A worker on each shard's log, its files small enough to close several times during the run.
    // How many passes it makes while commands run depends on the machine, so its catch-up pass at
    // the log's finish, which fails the run if it reports anything, is what shows it rolls.
    ScalingSettings rolled;
    rolled.shards = 2;
    rolled.durability = RunDurability::PROCESS;
    rolled.logParent = parent;
    rolled.rollWorker = true;
    rolled.logFileBytes = 512 + 800 * order_book::replay::logRecordBytes;  // about 800 records a file
    for (const BenchmarkKind kind : {BenchmarkKind::QUEUE, BenchmarkKind::GATEWAY}) {
        const Measurement run = runScaledOnce(workloads, kind, rolled);
        expect(run.outcome == totalOutcome(workloads), "a run with a roll-forward worker, which rolls without complaint");
    }
    expect(std::filesystem::is_empty(parent), "every shard's log directory is deleted");
    std::filesystem::remove_all(parent);
}

void testWhatARunRefuses() {
    const std::vector<Workload> workloads = spreadWorkloads(findWorkload("small-book"), 2000, 7, 4, Spread::UNIFORM);
    ScalingSettings settings;
    settings.shards = 2;
    settings.threads = 3;
    expectThrows([&] { static_cast<void>(runScaledOnce(workloads, BenchmarkKind::QUEUE, settings)); }, "more producers than shards");
    settings.threads = 1;
    expectThrows([&] { static_cast<void>(runScaledOnce(workloads, BenchmarkKind::CORE, settings)); }, "a kind that isn't threaded");
    settings.durability = RunDurability::PROCESS;
    expectThrows([&] { static_cast<void>(runScaledOnce(workloads, BenchmarkKind::SHARD, settings)); }, "a shard run with a log");
    settings.durability = RunDurability::NONE;
    settings.plan.cores = {0};
    expectThrows([&] { static_cast<void>(runScaledOnce(workloads, BenchmarkKind::SHARD, settings)); }, "a plan with too few cores");
    settings.plan.cores.clear();
    settings.framesPerPass = 0;
    expectThrows([&] { static_cast<void>(runScaledOnce(workloads, BenchmarkKind::GATEWAY, settings)); }, "a frame budget of 0");
    settings.framesPerPass = std::numeric_limits<std::size_t>::max();
    settings.threads = 5;
    expectThrows([&] { static_cast<void>(runScaledOnce(workloads, BenchmarkKind::GATEWAY, settings)); },
                 "more client threads than connections");
    settings.threads = 2;
    settings.rollWorker = true;
    expectThrows([&] { static_cast<void>(runScaledOnce(workloads, BenchmarkKind::QUEUE, settings)); }, "a worker with no log");
    settings.rollWorker = false;
    // One gateway thread of two can't be placed, so it never starts: the run fails, and the shards
    // still get every stop marker they wait for, so nothing hangs.
    settings.plan.cores = {0, 0, 0, 100000, 0, 0};
    expectThrows([&] { static_cast<void>(runScaledOnce(workloads, BenchmarkKind::GATEWAY, settings)); },
                 "a gateway thread that can't start fails the run without hanging it");
}

}  // namespace

int main() {
    testEveryCellDoesWhatItsBooksPredict();
    testLoggedAndUnboundedRuns();
    testWhatARunRefuses();
    std::cout << "benchmark scaling tests passed\n";
    return EXIT_SUCCESS;
}
