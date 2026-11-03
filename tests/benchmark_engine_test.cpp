#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "benchmark_runner.hpp"
#include "engine_process.hpp"
#include "order_book/invariants.hpp"
#include "workload.hpp"

// The engine kind against the shipped order_book_engine, a child process on port 0, so this
// carries the threaded label. Takes the engine's path. A checked build keeps to the small
// preloads, as benchmark_gateway_tests does.
namespace {

using namespace order_book::benchmark;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

template <typename Body>
std::string thrownBy(Body body) {
    try {
        body();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

RunSettings engineSettings(const std::filesystem::path& engine) {
    RunSettings settings;
    settings.engine = engine;
    return settings;
}

void testAnEngineRunDoesWhatAGatewayRunDoes(const std::filesystem::path& engine) {
    std::vector<const char*> names{"small-book", "quote-refresh"};
    if constexpr (!order_book::invariantChecksAfterCommands) {
        names.insert(names.end(), {"immediate", "self-trade", "pending-stops"});
    }
    for (const char* name : names) {
        const std::vector<Workload> workloads = instrumentWorkloads(findWorkload(name), 1000, 42, 1);
        const Measurement gateway = runInstrumentsOnce(workloads, BenchmarkKind::GATEWAY);
        const Measurement run = runInstrumentsOnce(workloads, BenchmarkKind::ENGINE, engineSettings(engine));
        expect(run.kind == BenchmarkKind::ENGINE && run.commands == gateway.commands, "an engine run takes the same commands");
        expect(run.outcome == gateway.outcome, std::string("and its frames say what the gateway's do, on ") + name);
        expect(run.window == defaultGatewayWindow && run.shards == 1, "under the default window and one shard, which it records");
    }
}

void testShardsAndWindowsChangeThePaceNotTheAnswers(const std::filesystem::path& engine) {
    const std::vector<Workload> workloads = instrumentWorkloads(findWorkload("quote-refresh"), 4000, 42, 4);
    for (const std::size_t shards : {std::size_t{1}, std::size_t{3}}) {
        for (const std::size_t window : {std::size_t{1}, std::size_t{256}}) {
            RunSettings settings = engineSettings(engine);
            settings.shards = shards;
            settings.window = window;
            const Measurement run = runInstrumentsOnce(workloads, BenchmarkKind::ENGINE, settings);
            expect(run.outcome == totalOutcome(workloads) && run.instruments == 4, "one connection per instrument, summing to the run");
            expect(run.shards == shards && run.window == window, "the run records its shards and window");
        }
    }
    RunSettings none = engineSettings(engine);
    none.window = 0;
    expect(!thrownBy([&] { static_cast<void>(runInstrumentsOnce(workloads, BenchmarkKind::ENGINE, none)); }).empty(),
           "a window of 0 can't make progress");
}

void testATamperedPredictionFails(const std::filesystem::path& engine) {
    std::vector<Workload> workloads = instrumentWorkloads(findWorkload("quote-refresh"), 1000, 42, 2);
    workloads[1].expected.trades += 1;
    const std::string why = thrownBy([&] { static_cast<void>(runInstrumentsOnce(workloads, BenchmarkKind::ENGINE, engineSettings(engine))); });
    expect(contains(why, "but the workload predicted"), "a prediction one trade out fails the run, not anything else: " + why);
}

// Only the engine's stop report can say a book grew, so this proves the report is read.
void testABookThatGrewFailsTheRun(const std::filesystem::path& engine) {
    std::vector<Workload> workloads = instrumentWorkloads(findWorkload("quote-refresh"), 1000, 42, 1);
    workloads[0].peak.orders = 1;
    workloads[0].peak.levels = 1;
    const std::string why = thrownBy([&] { static_cast<void>(runInstrumentsOnce(workloads, BenchmarkKind::ENGINE, engineSettings(engine))); });
    expect(contains(why, "grew past its capacity"), "a book past its capacity fails the run: " + why);
}

void testAMissingEngineIsNamed() {
    const std::vector<Workload> workloads = instrumentWorkloads(findWorkload("small-book"), 100, 42, 1);
    const std::string why = thrownBy(
        [&] { static_cast<void>(runInstrumentsOnce(workloads, BenchmarkKind::ENGINE, engineSettings("no/such/order_book_engine"))); });
    expect(contains(why, "--engine"), "a missing engine says how to name one: " + why);
}

void testStopReports() {
    const std::vector<EngineBookReport> books =
        parseStopReport("stopped\nbook B0 shard=0 orders=10 stops=2 levels=4 growths=0\r\nbook B10 shard=1 orders=7 stops=1 levels=3 growths=5\n"
                        "shard 0: result-growths=0\ntotal: books=2 capacity-growths=5\n");
    expect(books.size() == 2, "a stop report's book lines");
    expect(books[0].symbol == "B0" && books[0].shard == 0 && books[0].capacity.orders == 10 && books[0].capacity.stops == 2 &&
               books[0].capacity.levels == 4 && books[0].growths == 0,
           "a book line's fields, CRLF or not");
    expect(books[1].symbol == "B10" && books[1].shard == 1 && books[1].growths == 5, "and the next");
    expect(!thrownBy([] { static_cast<void>(parseStopReport("book B0 shard=0 orders=1 stops=1 levels=1 growths=0\n")); }).empty(),
           "a report with no stopped line is refused");
    expect(!thrownBy([] { static_cast<void>(parseStopReport("stopped\nbook B0 shard=0 orders=1\n")); }).empty(),
           "a book line missing a field is refused");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: benchmark_engine_tests ENGINE" << std::endl;
        return EXIT_FAILURE;
    }
    const std::filesystem::path engine = argv[1];
    testStopReports();
    testAMissingEngineIsNamed();
    testAnEngineRunDoesWhatAGatewayRunDoes(engine);
    testShardsAndWindowsChangeThePaceNotTheAnswers(engine);
    testATamperedPredictionFails(engine);
    testABookThatGrewFailsTheRun(engine);
    std::cout << "benchmark engine tests passed" << std::endl;
    return EXIT_SUCCESS;
}
