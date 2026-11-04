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

// The gateway kind over real loopback sockets and real threads, so this carries the threaded
// label, and ThreadSanitizer and Arm run it. A checked build keeps to the small preloads, as
// benchmark_queue_tests does.
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

void testAGatewayRunDoesWhatACoreRunDoes() {
    std::vector<const char*> names{"small-book", "quote-refresh"};
    if constexpr (!order_book::invariantChecksAfterCommands) {
        names.insert(names.end(), {"immediate", "self-trade", "pending-stops"});
    }
    for (const char* name : names) {
        const Workload workload = generateWorkload(findWorkload(name), 1000, 42);
        const Measurement core = runOnce(workload, BenchmarkKind::CORE);
        const Measurement gateway = runOnce(workload, BenchmarkKind::GATEWAY);
        expect(gateway.kind == BenchmarkKind::GATEWAY && gateway.commands == core.commands, "a gateway run takes the same commands");
        expect(gateway.outcome == core.outcome, std::string("and does what the book does directly, on ") + name);
        expect(gateway.window == defaultGatewayWindow, "under the default window, which it records");
    }
}

void testAnyWindowGivesTheSameAnswers() {
    const std::vector<Workload> workloads = instrumentWorkloads(findWorkload("quote-refresh"), 600, 42, 1);
    for (const std::size_t window : {std::size_t{1}, std::size_t{7}, std::size_t{100000}}) {
        const Measurement run = runInstrumentsOnce(workloads, BenchmarkKind::GATEWAY, window);
        expect(run.outcome == totalOutcome(workloads) && run.window == window, "the window changes the pace, not the answers");
    }
    expectThrows([&] { static_cast<void>(runInstrumentsOnce(workloads, BenchmarkKind::GATEWAY, 0)); }, "a window of 0 can't make progress");
}

void testSeveralConnectionsSumToTheRun() {
    const std::vector<Workload> workloads = instrumentWorkloads(findWorkload("quote-refresh"), 4000, 42, 4);
    const Measurement run = runInstrumentsOnce(workloads, BenchmarkKind::GATEWAY);
    expect(run.outcome == totalOutcome(workloads) && run.instruments == 4, "one connection per instrument, summing to the run");
}

void testATamperedPredictionFails() {
    std::vector<Workload> workloads = instrumentWorkloads(findWorkload("quote-refresh"), 1000, 42, 2);
    workloads[1].expected.trades += 1;
    expectThrows([&] { static_cast<void>(runInstrumentsOnce(workloads, BenchmarkKind::GATEWAY)); }, "a prediction one trade out fails the run");
}

void testABookThatGrewFailsTheRun() {
    std::vector<Workload> workloads = instrumentWorkloads(findWorkload("quote-refresh"), 1000, 42, 1);
    workloads[0].peak.orders = 1;
    workloads[0].peak.levels = 1;
    expectThrows([&] { static_cast<void>(runInstrumentsOnce(workloads, BenchmarkKind::GATEWAY)); }, "a book past its capacity fails the run");
}

// At process and machine, the shard logs every timed command to real files, which the run deletes,
// and the gateway thread holds each response until its command is durable.
void testADurableGatewayRunDoesWhatACoreRunDoes() {
    const std::vector<Workload> workloads = instrumentWorkloads(findWorkload("quote-refresh"), 1000, 42, 2);
    for (const RunDurability durability : {RunDurability::PROCESS, RunDurability::MACHINE}) {
        const std::filesystem::path parent =
            std::filesystem::temp_directory_path() / (std::string("order_book_benchmark_gateway_tests_") + durabilityName(durability));
        std::filesystem::remove_all(parent);
        std::filesystem::create_directories(parent);
        RunSettings settings;
        settings.durability = durability;
        settings.logParent = parent;
        const Measurement run = runInstrumentsOnce(workloads, BenchmarkKind::GATEWAY, settings);
        expect(run.outcome == totalOutcome(workloads) && run.durability == durability,
               std::string("a gateway run at ") + durabilityName(durability) + " does what the books predict");
        expect(std::filesystem::is_empty(parent), "and deletes its log directory");
        std::filesystem::remove_all(parent);
    }
}

void testALogThatCantBeMadeFailsTheRun() {
    const std::vector<Workload> workloads = instrumentWorkloads(findWorkload("small-book"), 100, 42, 1);
    RunSettings settings;
    settings.durability = RunDurability::MACHINE;
    settings.logParent = std::filesystem::temp_directory_path() / "order_book_no_such_directory" / "deeper";
    expectThrows([&] { static_cast<void>(runInstrumentsOnce(workloads, BenchmarkKind::GATEWAY, settings)); },
                 "a log directory that can't be made fails the run");
}

}  // namespace

int main() {
    testAGatewayRunDoesWhatACoreRunDoes();
    testAnyWindowGivesTheSameAnswers();
    testSeveralConnectionsSumToTheRun();
    testATamperedPredictionFails();
    testABookThatGrewFailsTheRun();
    testADurableGatewayRunDoesWhatACoreRunDoes();
    testALogThatCantBeMadeFailsTheRun();
    std::cout << "benchmark gateway tests passed" << std::endl;
    return EXIT_SUCCESS;
}
