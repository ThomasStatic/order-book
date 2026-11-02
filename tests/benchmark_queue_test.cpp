#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "benchmark_runner.hpp"
#include "order_book/invariants.hpp"
#include "workload.hpp"

// The queue kind on real threads, so this carries the threaded label, and ThreadSanitizer and Arm
// run it. It keeps to workloads with small preloads, which a checked build can afford.
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
    // A large preload is out of reach with a book's whole check after every command.
    std::vector<const char*> names{"small-book", "quote-refresh", "immediate", "self-trade"};
    if constexpr (!order_book::invariantChecksAfterCommands) {
        names.push_back("pending-stops");
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

// A matching thread that fails after it has told the producer it is ready must not leave the
// producer waiting for ever: a book that outgrows its capacity is refused once it stops.
void testAFailedMatchingThreadDoesNotHang() {
    std::vector<Workload> workloads = instrumentWorkloads(findWorkload("quote-refresh"), 1000, 42, 1);
    workloads[0].peak.orders = 1;
    workloads[0].peak.levels = 1;
    expectThrows([&] { static_cast<void>(runInstrumentsOnce(workloads, BenchmarkKind::QUEUE)); },
                 "a book past its capacity fails the run, and finishing it returns");
}

void testASetUpFailureReachesTheCaller() {
    const std::vector<Workload> workloads = instrumentWorkloads(findWorkload("small-book"), 100, 42, 1);
    expectThrows([&] { QueueRun run(workloads, [] { throw std::runtime_error("could not pin"); }); },
                 "a matching thread that can't start says so");
}

}  // namespace

int main() {
    testAQueueRunDoesWhatACoreRunDoes();
    testPredictionsSumToTheRun();
    testATamperedPredictionFails();
    testAFailedMatchingThreadDoesNotHang();
    testASetUpFailureReachesTheCaller();
    std::cout << "benchmark queue tests passed" << std::endl;
    return EXIT_SUCCESS;
}
