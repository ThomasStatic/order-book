#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "order_book/runtime/matching_loop.hpp"
#include "threaded_run.hpp"

// Layer 3: real gateway and matching threads on the in-memory SocketSet, against the
// single-threaded run, and the triage of a failure. A lost wake-up hangs here, so CTest's timeout
// is part of the test. See docs/internals/randomized-tests.md.
namespace {

using namespace order_book::engine_testing;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

// N = 1, 2, 4 and M = 1, 2, with rings of capacity 2 or the engine's, at spin window 0 or the
// default: 24 runs, each instrument's answers equal to the single-threaded run's.
void testRealThreadsMatchTheSingleThreadedRun() {
    std::size_t sleeps = 0;
    std::uint64_t seed = 1;
    for (const std::uint32_t shards : {1U, 2U, 4U}) {
        for (const std::size_t gatewayThreads : {std::size_t{1}, std::size_t{2}}) {
            for (const std::size_t ringSlots : {std::size_t{2}, std::size_t{0}}) {
                for (const std::chrono::nanoseconds spinWindow : {std::chrono::nanoseconds::zero(), order_book::runtime::defaultSpinWindow}) {
                    ThreadedConfig config;
                    config.seed = seed++;
                    config.shards = shards;
                    config.gatewayThreads = gatewayThreads;
                    config.ringSlots = ringSlots;
                    config.spinWindow = spinWindow;
                    const ThreadedResult result = runThreadedSchedule(config);
                    if (!result.passed()) {
                        const ThreadedTriage triage = triageThreadedFailure(config, result);
                        expect(false, triage.report);
                    }
                    if (spinWindow == std::chrono::nanoseconds::zero()) {
                        sleeps += result.gatewaySleeps;
                    }
                }
            }
        }
    }
    expect(sleeps > 0, "at spin window 0 the gateway threads slept, so a lost wake-up would have hung");
}

ThreadedConfig planted(ThreadedPlant plant) {
    ThreadedConfig config;
    config.seed = 5;
    config.commands = 300;
    config.shards = 2;
    config.gatewayThreads = 2;
    config.ringSlots = 2;
    config.plant = plant;
    return config;
}

void testALogicBugIsLeftToTheSingleThreadedHarness() {
    const ThreadedConfig config = planted(ThreadedPlant::SHARD);
    const ThreadedResult result = runThreadedSchedule(config);
    expect(!result.passed(), "the planted run fails");
    const ThreadedTriage triage = triageThreadedFailure(config, result, 5);
    expect(triage.verdict == ThreadedTriage::Verdict::LOGIC_BUG, "Shard::process fails too, so it is a logic bug:\n" + triage.report);
    expect(!triage.stepperSeed && triage.keptLines.empty(), "with no stepper run and no shrinking");
    expect(triage.report.find("engine_randomized_tests --seed 5 --commands 300 --instruments 8 --shards 2") != std::string::npos,
           "and says how to rerun it single-threaded:\n" + triage.report);
}

void testADeterministicReproductionIsShrunk() {
    const ThreadedConfig config = planted(ThreadedPlant::EVERY_RUN);
    const ThreadedResult result = runThreadedSchedule(config);
    expect(!result.passed() && result.differences.front().starts_with("SYM0: answer 2 of"),
           "the planted run fails on instrument 0, after its first answer: " + result.failure.value_or(""));
    const ThreadedTriage triage = triageThreadedFailure(config, result, 5);
    expect(triage.verdict == ThreadedTriage::Verdict::DETERMINISTIC && triage.stepperSeed == 1U,
           "the first stepper schedule reproduces it:\n" + triage.report);
    expect(triage.shrunkInFull && triage.keptLines.size() == 1, "and it shrinks to the one line it needs:\n" + triage.report);
    expect(triage.report.find("--stream-seed 5 ") != std::string::npos && triage.report.find("--lines ") != std::string::npos,
           "naming the run's stream and the lines kept:\n" + triage.report);
    expect(triage.report.find("SYM0: answer 2 of") != std::string::npos, "having saved the first difference:\n" + triage.report);
}

void testATimingDependentFailureIsReportedUnshrunk() {
    const ThreadedConfig config = planted(ThreadedPlant::THREADS_ONLY);
    const ThreadedResult result = runThreadedSchedule(config);
    expect(!result.passed(), "the planted run fails");
    const ThreadedTriage triage = triageThreadedFailure(config, result, 5);
    expect(triage.verdict == ThreadedTriage::Verdict::TIMING_DEPENDENT && !triage.stepperSeed && triage.keptLines.empty(),
           "no stepper schedule reproduces it, so it isn't shrunk:\n" + triage.report);
    expect(triage.report.find("none of 5 stepper schedules") != std::string::npos && triage.report.find(result.log) != std::string::npos,
           "and the report carries the run's own log:\n" + triage.report);
    expect(triage.report.find("--threads run --seed 5 --commands 300 --instruments 8 --shards 2 --gateway-threads 2 --ring-slots 2") !=
               std::string::npos,
           "and what reruns it:\n" + triage.report);
}

}  // namespace

int main() {
    testRealThreadsMatchTheSingleThreadedRun();
    testALogicBugIsLeftToTheSingleThreadedHarness();
    testADeterministicReproductionIsShrunk();
    testATimingDependentFailureIsReportedUnshrunk();
    std::cout << "engine thread tests passed" << std::endl;
    return EXIT_SUCCESS;
}
