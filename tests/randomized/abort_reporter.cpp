#include "abort_reporter.hpp"

#include <csignal>
#include <cstdio>

namespace order_book::randomized {

namespace {

struct AbortContext {
    bool active = false;
    std::string description;
    std::string reproduce;
    bool appendCount = false;
    bool commandStarted = false;
    std::size_t index = 0;
    Command command;
};

AbortContext context;

// SIGABRT reaches here synchronously from the engine's own std::abort, called on this thread
// after it has finished printing its violation, not from inside an allocator or stdio call;
// so formatting and printing here is safe in practice, though not async-signal-safe in general.
extern "C" void reportAbort(int) {
    if (!context.active) {
        return;
    }
    std::fprintf(stderr, "\nRANDOMIZED TEST ABORTED\n\n");
    std::fprintf(stderr, "The engine aborted (its message is above) while running %s.\n", context.description.c_str());
    if (context.commandStarted) {
        std::fprintf(stderr, "Command index: %zu (0-based)\n", context.index);
        std::fprintf(stderr, "Command:       %s\n", formatCommand(context.command).c_str());
    }
    if (context.appendCount) {
        std::fprintf(stderr, "Reproduce:     %s%zu\n", context.reproduce.c_str(),
                     context.commandStarted ? context.index + 1 : 0);
    } else {
        std::fprintf(stderr, "Reproduce:     %s\n", context.reproduce.c_str());
    }
    std::fprintf(stderr,
                 "\nIn this build the engine checks its own invariants after every command and aborts on a\n"
                 "violation, so the harness could not report, save, or minimize the stream. Rerun the line\n"
                 "above in a build without automatic checks (Release, or ORDER_BOOK_INVARIANT_CHECKS=OFF):\n"
                 "the harness then runs the same checks itself and does all three.\n");
    std::fflush(stderr);
}

}  // namespace

void installAbortReporter() {
    std::signal(SIGABRT, reportAbort);
}

void setAbortContext(std::string description, std::string reproduce, bool appendCount) {
    context.description = std::move(description);
    context.reproduce = std::move(reproduce);
    context.appendCount = appendCount;
    context.commandStarted = false;
    context.active = true;
}

void clearAbortContext() {
    context.active = false;
}

void noteCommand(std::size_t index, const Command& command) {
    context.index = index;
    context.command = command;
    context.commandStarted = true;
}

}  // namespace order_book::randomized
