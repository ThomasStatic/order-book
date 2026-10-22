#pragma once

#include <cstddef>
#include <string>

#include "command.hpp"

namespace order_book::randomized {

// With automatic invariant checks on (Debug builds by default), the engine aborts at its own
// command boundary on a broken invariant, before the harness can check, report, save, or
// minimize anything. This keeps enough context to say which run and command it was and how to
// reproduce it, and prints that from a SIGABRT handler.

// Installs the handler; call once from main.
void installAbortReporter();

// Describes the run now in progress. `reproduce` is a command line that reruns it; when
// appendCount is set, the number of commands executed so far is appended to it.
void setAbortContext(std::string description, std::string reproduce, bool appendCount);
void clearAbortContext();

// Called by CommandReplayer before each command.
void noteCommand(std::size_t index, const Command& command);

}  // namespace order_book::randomized
