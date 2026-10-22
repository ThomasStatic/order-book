#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "command.hpp"
#include "command_replayer.hpp"
#include "shrink.hpp"

namespace order_book::randomized {

// Where a run's commands came from: a seed and profile, or a saved stream.
struct RunOrigin {
    std::string program;  // how to invoke randomized_tests, for replay instructions
    std::optional<std::uint64_t> seed;
    std::string profileName;
    std::string profileConfig;
    std::size_t plannedCommands = 0;
    std::string replayedFile;  // set instead of seed and profile for a replayed stream
};

// What was kept after a failure. An empty path means that stream was not saved.
struct PreservedStreams {
    std::string prefixPath;
    std::string minimizedPath;
    std::optional<ShrinkResult> shrunk;
    std::string problem;  // why something could not be saved or shrunk, if anything
};

// The command line that regenerates a seeded run's first `commandCount` commands.
std::string seedReplayCommand(const RunOrigin& origin, std::size_t commandCount);

// The full report: where the commands came from, the failing command and its index, the
// failure and the expectation or invariant it broke, the book around it, the commands that
// led there, the saved streams, and how to replay them.
std::string formatFailureReport(const RunOrigin& origin, const Failure& failure, const std::vector<Command>& prefix,
                                const PreservedStreams& preserved);

}  // namespace order_book::randomized
