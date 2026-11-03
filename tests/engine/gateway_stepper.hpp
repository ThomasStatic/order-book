#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine_harness.hpp"

// The stepper: one thread plays each actor of the gateway path, the clients, each GatewayThread
// on the in-memory SocketSet, and the shards' MatchingLoops, choosing the next from a seeded source,
// on rings of capacity 2 with reads and writes split anywhere. A schedule is its seed and its
// config, so a failing one is a deterministic reproduction. See docs/internals/randomized-tests.md.
namespace order_book::engine_testing {

    struct StepperConfig {
        std::uint64_t seed = 1;                  // the schedule's, and the stream's unless streamSeed is set
        std::optional<std::uint64_t> streamSeed;  // so many schedules can run one stream
        std::size_t commands = 1000;
        std::size_t instruments = 6;
        std::uint32_t shards = 2;
        std::size_t connections = 3;  // each owning the instruments whose index it divides into
        std::size_t gatewayThreads = 1;
        bool pinned = false;  // instrument k on shard k mod shards, as the real-thread runs place them
        bool plantExtraAnswer = false;  // see StreamShape, for testing the real-thread triage
        // Land a stop at a step chosen from the seed, which each gateway thread begins at its
        // next pass, as runGateway does.
        bool stop = false;
        // Every shard logs to in-memory storage, its log writer is one more actor, and no
        // response may be released before its command is in the storage.
        bool log = false;
    };

    struct StepperResult {
        std::optional<std::string> failure;
        std::size_t sleeps = 0;           // checked, each one, against the interest it was given
        std::size_t waitsInPlace = 0;     // a matching thread on a full response ring
        std::size_t commandsRun = 0;
        std::size_t commandsRefused = 0;  // with the retry code, by a stop

        bool passed() const { return !failure.has_value(); }
    };

    // Without a stop, every instrument's answers must equal a single-threaded run of its commands.
    // With one, every command must run, be refused with the retry code, or never be read, in that
    // order per connection, and what ran must equal a single-threaded run of just those commands.
    // Every sleep must leave nothing in the sleeping thread's rings, every connection it holds with
    // bytes to move watched, and it woken if a connection was handed to it. Connection i must end
    // on gateway thread i mod M. With a log, after every step no client may hold an answer to a
    // command whose record isn't yet in the storage, the durable position may not pass what the
    // storage holds, and each instrument's log must end holding exactly the commands that ran, in
    // order, with a stop's files closed and reading to their end.
    StepperResult runStepperSchedule(const StepperConfig& config);
    // The same schedule on these lines in place of the seed's own stream, which is how a failing
    // schedule shrinks with its seed held fixed. Every line's instrument must be one of config's.
    StepperResult runStepperSchedule(const StepperConfig& config, const std::vector<EngineCommand>& lines);

}  // namespace order_book::engine_testing
