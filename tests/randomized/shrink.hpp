#pragma once

#include <cstddef>
#include <vector>

#include "command.hpp"
#include "command_replayer.hpp"

namespace order_book::randomized {

struct ShrinkResult {
    std::vector<Command> commands;
    Failure failure;           // what the shrunk stream fails with
    std::size_t attempts = 0;  // candidate streams replayed
    // Whether it reached a stream from which removing any one command loses the failure;
    // false if the attempt budget ran out first (the stream still fails, but may shrink further).
    bool complete = false;
};

// Removes commands from a failing stream while it still fails the same way (Failure::sameKindAs:
// the same failure type and violated invariant), by delta debugging: try dropping each of n
// chunks, keep any drop that still fails, and refine n until the chunks are single commands.
// Each candidate is replayed from an empty book, so a candidate's remaining commands are
// checked against the model afresh; after each success the stream is cut at its new failure.
ShrinkResult shrinkFailingStream(const std::vector<Command>& commands, const Failure& failure,
                                 const CommandReplayer& replayer, std::size_t maxAttempts = 20000);

}  // namespace order_book::randomized
