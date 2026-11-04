#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "connection_streams.hpp"
#include "engine_harness.hpp"
#include "order_book/command.hpp"
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

template <typename Function>
void expectThrows(Function function, const std::string& message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    expect(false, message + " should throw");
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

// Two connections on each instrument, one on each of M = 2 gateway threads, at N = 1, 2, 4, with
// rings of capacity 2 or the engine's, at spin window 0: the take order decides each instrument's
// answers, so each is held to a replay of the run's own log.
void testSeveralConnectionsOnOneInstrumentMatchTheirLog() {
    std::uint64_t seed = 101;
    for (const std::uint32_t shards : {1U, 2U, 4U}) {
        for (const std::size_t ringSlots : {std::size_t{2}, std::size_t{0}}) {
            ThreadedConfig config;
            config.seed = seed++;
            config.commands = 3000;
            config.shards = shards;
            config.gatewayThreads = 2;
            config.ringSlots = ringSlots;
            config.connectionsPerInstrument = 2;
            const ThreadedResult result = runThreadedSchedule(config);
            if (!result.passed()) {
                expect(false, triageThreadedFailure(config, result).report);
            }
        }
    }
}

// The log's replay decides what each connection should have heard, so a log that doesn't hold
// what the connections sent can't stand for them.
void testALogNoConnectionSentIsRefused() {
    StreamShape shape{4, 2, 0, true, false, 2};
    EngineSeedConfig generation;
    generation.seed = 9;
    generation.instruments = shape.instruments;
    generation.shards = shape.shards;
    generation.commands = 400;
    generation.logWritePercent = 0;
    generation.crashes = false;
    const ConnectionStreams streams = connectionStreams(shape, generateEngineStream(generation));
    expect(streams.bytes.size() == 8, "two connections on each of 4 instruments");
    std::map<std::string, std::vector<order_book::replay::Command>> logged;
    for (const StreamCommand& command : streams.commands) {
        expect(command.connection / 2 == static_cast<std::size_t>(command.accepted.symbol.text().back() - '0'),
               "each instrument's commands go to its own two connections");
        logged[command.accepted.symbol.text()].push_back(command.accepted.command);
    }
    std::size_t connectionsUsed = 0;
    for (const std::size_t answers : streams.answers) {
        connectionsUsed += answers > 0 ? 1 : 0;
    }
    expect(connectionsUsed == 8, "every connection carries commands");
    const ConnectionStreams fromLog = expectedFromLog(streams, logged);
    expect(fromLog.expected.size() == 4, "and the log in the order they were sent stands for them");

    std::map<std::string, std::vector<order_book::replay::Command>> missing = logged;
    missing.begin()->second.pop_back();
    expectThrows([&] { static_cast<void>(expectedFromLog(streams, missing)); }, "a log missing a command");
    std::map<std::string, std::vector<order_book::replay::Command>> extra = logged;
    extra.begin()->second.push_back(order_book::replay::CancelCommand{4000000000U});
    expectThrows([&] { static_cast<void>(expectedFromLog(streams, extra)); }, "a log with a command nobody sent");
    // A connection's two commands swapped: the log's order breaks the order it sent them in.
    std::map<std::string, std::vector<order_book::replay::Command>> reordered = logged;
    std::vector<order_book::replay::Command>& first = reordered.begin()->second;
    const std::string symbol = reordered.begin()->first;
    std::size_t a = first.size();
    std::size_t b = first.size();
    std::size_t index = 0;
    for (const StreamCommand& command : streams.commands) {
        if (command.accepted.symbol.text() != symbol) {
            continue;
        }
        if (command.connection % 2 == 0 && a == first.size()) {
            a = index;
        } else if (command.connection % 2 == 0 && b == first.size() && !(first[a] == command.accepted.command)) {
            b = index;
        }
        ++index;
    }
    expect(b < first.size(), "a connection with two different commands");
    std::swap(first[a], first[b]);
    expectThrows([&] { static_cast<void>(expectedFromLog(streams, reordered)); }, "a log out of a connection's order");

    // The replay goes through the model too, so a book that answers otherwise than the model is
    // a logic bug: here the model is told of a different quantity than the book runs.
    ConnectionStreams tampered = streams;
    bool changed = false;
    for (StreamCommand& command : tampered.commands) {
        if (!changed && command.converted.type == order_book::randomized::CommandType::SUBMIT && command.converted.quantity > 1) {
            command.converted.quantity -= 1;
            changed = true;
        }
    }
    expect(changed, "a submission to tamper with");
    bool diverged = false;
    try {
        static_cast<void>(expectedFromLog(tampered, logged));
    } catch (const LogReplayDiverged&) {
        diverged = true;
    }
    expect(diverged, "a book and model that disagree on the log's replay are a logic bug");
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
    testALogNoConnectionSentIsRefused();
    testRealThreadsMatchTheSingleThreadedRun();
    testSeveralConnectionsOnOneInstrumentMatchTheirLog();
    testALogicBugIsLeftToTheSingleThreadedHarness();
    testADeterministicReproductionIsShrunk();
    testATimingDependentFailureIsReportedUnshrunk();
    std::cout << "engine thread tests passed" << std::endl;
    return EXIT_SUCCESS;
}
