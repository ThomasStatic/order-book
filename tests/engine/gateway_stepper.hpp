#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// The stepper: one thread plays each actor of the gateway path, the clients, a GatewayThread on
// the in-memory SocketSet, and the shards' MatchingLoops, choosing the next from a seeded source,
// on rings of capacity 2 with reads and writes split anywhere. A schedule is its seed and its
// config, so a failing one is a deterministic reproduction. See docs/internals/randomized-tests.md.
namespace order_book::engine_testing {

    struct StepperConfig {
        std::uint64_t seed = 1;
        std::size_t commands = 1000;
        std::size_t instruments = 6;
        std::uint32_t shards = 2;
        std::size_t connections = 3;  // each owning the instruments whose index it divides into
        bool stop = false;            // land a stop at a step chosen from the seed
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
    // Every sleep must leave nothing in a ring and every connection with bytes to move watched.
    StepperResult runStepperSchedule(const StepperConfig& config);

}  // namespace order_book::engine_testing
