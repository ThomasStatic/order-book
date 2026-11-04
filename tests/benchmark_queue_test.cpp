#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "benchmark_runner.hpp"
#include "order_book/invariants.hpp"
#include "workload.hpp"

// The queue kind on real threads, so this carries the threaded label, and ThreadSanitizer and Arm
// run it. A checked build gets only the workloads with small preloads: it checks the whole book
// after every command, so the others take minutes under ThreadSanitizer. Clang's ThreadSanitizer
// job is optimized, and runs them all.
namespace {

using namespace order_book::benchmark;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

template <typename Body>
void expectThrows(Body body, const std::string& message) {
    try {
        body();
    } catch (const std::exception&) {
        return;
    }
    expect(false, message);
}

void testAQueueRunDoesWhatACoreRunDoes() {
    std::vector<const char*> names{"small-book", "quote-refresh"};
    if constexpr (!order_book::invariantChecksAfterCommands) {
        names.insert(names.end(), {"immediate", "self-trade", "pending-stops"});
    }
    for (const char* name : names) {
        const Workload workload = generateWorkload(findWorkload(name), 1000, 42);
        const Measurement core = runOnce(workload, BenchmarkKind::CORE);
        const Measurement queue = runOnce(workload, BenchmarkKind::QUEUE);
        expect(queue.kind == BenchmarkKind::QUEUE && queue.commands == core.commands, "a queue run takes the same commands");
        expect(queue.outcome == core.outcome, "and does what the book does directly");
    }
}

void testPredictionsSumToTheRun() {
    const std::vector<Workload> workloads = instrumentWorkloads(findWorkload("quote-refresh"), 4000, 42, 4);
    const Measurement run = runInstrumentsOnce(workloads, BenchmarkKind::QUEUE);
    expect(run.outcome == totalOutcome(workloads) && run.instruments == 4, "the predictions sum to the run's total");
}

void testATamperedPredictionFails() {
    std::vector<Workload> workloads = instrumentWorkloads(findWorkload("quote-refresh"), 1000, 42, 2);
    workloads[1].expected.trades += 1;
    expectThrows([&] { static_cast<void>(runInstrumentsOnce(workloads, BenchmarkKind::QUEUE)); },
                 "a prediction one trade out fails the run on real threads too");
}

// The book can only be checked on the thread that owns it, once that thread has stopped, so this
// failure has to cross back to the producer through finish.
void testABookThatGrewFailsTheRun() {
    std::vector<Workload> workloads = instrumentWorkloads(findWorkload("quote-refresh"), 1000, 42, 1);
    workloads[0].peak.orders = 1;
    workloads[0].peak.levels = 1;
    expectThrows([&] { static_cast<void>(runInstrumentsOnce(workloads, BenchmarkKind::QUEUE)); },
                 "a book past its capacity fails the run");
}

// Everything that can fail before the loop runs reaches the constructor, so no producer is ever
// left waiting on a thread that never started.
void testASetUpFailureReachesTheCaller() {
    const std::vector<Workload> workloads = instrumentWorkloads(findWorkload("small-book"), 100, 42, 1);
    expectThrows([&] { QueueRun run(workloads, [] { throw std::runtime_error("could not pin"); }); },
                 "a matching thread that can't start says so");
}

// A directory of its own for this test's run directories, empty to start.
std::filesystem::path logParent(const std::string& name) {
    const std::filesystem::path parent = std::filesystem::temp_directory_path() / ("order_book_benchmark_queue_tests_" + name);
    std::filesystem::remove_all(parent);
    std::filesystem::create_directories(parent);
    return parent;
}

// At process and machine, the shard logs every timed command to real files, which a run deletes,
// and the producer waits at hold markers as a gateway thread does.
void testADurableQueueRunDoesWhatACoreRunDoes() {
    const Workload workload = generateWorkload(findWorkload("small-book"), 1000, 42);
    const Measurement core = runOnce(workload, BenchmarkKind::CORE);
    for (const RunDurability durability : {RunDurability::PROCESS, RunDurability::MACHINE}) {
        const std::filesystem::path parent = logParent(durabilityName(durability));
        RunSettings settings;
        settings.durability = durability;
        settings.logParent = parent;
        const Measurement queue = runInstrumentsOnce({workload}, BenchmarkKind::QUEUE, settings);
        expect(queue.outcome == core.outcome && queue.durability == durability, std::string("a queue run at ") +
                                                                                    durabilityName(durability) + " does what the book does");
        expect(std::filesystem::is_empty(parent), "and deletes its log directory");

        const std::vector<Workload> one{workload};
        QueueRun run(one, {}, durability, parent);
        run.run();
        run.finish();
        run.check();
        // Write-through takes long enough that responses always meet a hold; at process the
        // writer can win every race, so only machine is asked.
        expect(durability == RunDurability::PROCESS || run.holdsWaited() > 0, "its responses waited for the log");
        std::filesystem::remove_all(parent);
    }
}

void testANoLogRunNeverHolds() {
    const std::vector<Workload> workloads = instrumentWorkloads(findWorkload("small-book"), 1000, 42, 1);
    QueueRun run(workloads);
    run.run();
    run.finish();
    expect(run.holdsWaited() == 0, "a shard with no log puts no hold markers out");
}

// Its directory can't be made, so the run fails before anything is timed, and the matching thread
// it started is stopped, not left waiting.
void testALogThatCantBeMadeFailsTheRun() {
    const std::vector<Workload> workloads = instrumentWorkloads(findWorkload("small-book"), 100, 42, 1);
    RunSettings settings;
    settings.durability = RunDurability::PROCESS;
    settings.logParent = std::filesystem::temp_directory_path() / "order_book_no_such_directory" / "deeper";
    expectThrows([&] { static_cast<void>(runInstrumentsOnce(workloads, BenchmarkKind::QUEUE, settings)); },
                 "a log directory that can't be made fails the run");
}

}  // namespace

int main() {
    testAQueueRunDoesWhatACoreRunDoes();
    testPredictionsSumToTheRun();
    testATamperedPredictionFails();
    testABookThatGrewFailsTheRun();
    testASetUpFailureReachesTheCaller();
    testADurableQueueRunDoesWhatACoreRunDoes();
    testANoLogRunNeverHolds();
    testALogThatCantBeMadeFailsTheRun();
    std::cout << "benchmark queue tests passed" << std::endl;
    return EXIT_SUCCESS;
}
