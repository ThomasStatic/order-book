#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "order_book/runtime/matching_loop.hpp"

// Layer 3: real GatewayThreads and real matching threads on the in-memory SocketSet, one
// connection per instrument, each instrument's answers held to a single-threaded run. A failure
// is never shrunk directly: triageThreadedFailure saves it, reruns it through Shard::process and
// then the stepper, and shrinks only a deterministic reproduction. See
// docs/internals/randomized-tests.md.
namespace order_book::engine_testing {

    // What to plant, so the triage's three verdicts can each be shown.
    enum class ThreadedPlant {
        NONE,
        SHARD,           // a divergence Shard::process itself shows: a logic bug
        EVERY_RUN,       // an answer every run expects and none gives: the stepper reproduces it
        THREADS_ONLY,    // a difference only the real-thread run reports: timing-dependent
    };

    struct ThreadedConfig {
        std::uint64_t seed = 1;
        std::size_t commands = 5000;
        std::size_t instruments = 8;  // placed on shard k mod shards, each with its own connection
        std::uint32_t shards = 1;
        std::size_t gatewayThreads = 1;
        std::size_t ringSlots = 0;  // every ring's slots, or 0 for the engine's defaults
        std::chrono::nanoseconds spinWindow = std::chrono::nanoseconds::zero();  // for every thread
        ThreadedPlant plant = ThreadedPlant::NONE;
        // Above 1, each instrument has this many connections, a command going to the one of its
        // order ID mod this, so an instrument's connections land on different gateway threads.
        // The take order then decides its answers, which are held to a replay of its own log.
        std::size_t connectionsPerInstrument = 1;
    };

    struct ThreadedResult {
        std::optional<std::string> failure;
        std::vector<std::string> differences;  // the first differing answer per instrument
        std::string log;                       // the run's own, for a report
        std::size_t gatewaySleeps = 0;         // across the gateway threads' sets
        bool logReplayDiverged = false;        // the log, replayed through a book and the model, disagreed

        bool passed() const { return !failure.has_value(); }
    };

    // Runs every command to its answer, then stops the engine as runEngine does. A lost wake-up
    // hangs here, which is why its test runs under a CTest timeout.
    ThreadedResult runThreadedSchedule(const ThreadedConfig& config);

    struct ThreadedTriage {
        enum class Verdict { LOGIC_BUG, DETERMINISTIC, TIMING_DEPENDENT };

        Verdict verdict;
        std::optional<std::uint64_t> stepperSeed;  // the schedule that reproduced it
        std::vector<std::size_t> keptLines;        // shrunk: the seed's stream's lines that still fail
        bool shrunkInFull = false;                 // false if the attempt budget ran out first
        std::string report;                        // everything saved, to print or write
    };

    // A failed run's triage: the failure saved with its config and differences; then the same
    // stream through single-threaded Shard::process, where a failure is a logic bug; then
    // stepperSchedules stepper schedules on it with rings of capacity 2, the first failing one a
    // deterministic reproduction, shrunk with its schedule's seed held fixed; and otherwise
    // timing-dependent and unshrunk, with the run's own log.
    ThreadedTriage triageThreadedFailure(const ThreadedConfig& config, const ThreadedResult& result, std::size_t stepperSchedules = 1000,
                                         std::size_t shrinkAttempts = 2000);

    // The command that runs this config alone.
    std::string threadedReproduction(const ThreadedConfig& config);

}  // namespace order_book::engine_testing
