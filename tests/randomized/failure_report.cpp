#include "failure_report.hpp"

#include <algorithm>
#include <sstream>

#include "command_generator.hpp"

namespace order_book::randomized {

namespace {

// How many commands before the failing one the report lists, and how long a minimized
// stream may be and still be listed in full.
constexpr std::size_t precedingShown = 8;
constexpr std::size_t minimizedShown = 40;

std::string indent(const std::string& text, const std::string& prefix) {
    std::string indented;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        indented += prefix + line + "\n";
    }
    return indented;
}

}  // namespace

std::string seedReplayCommand(const RunOrigin& origin, std::size_t commandCount) {
    return origin.program + " --seed " + std::to_string(origin.seed.value_or(0)) + " --profile " +
           origin.profileName + " --commands " + std::to_string(commandCount);
}

std::string formatFailureReport(const RunOrigin& origin, const Failure& failure, const std::vector<Command>& prefix,
                                const PreservedStreams& preserved) {
    std::ostringstream out;
    out << "RANDOMIZED TEST FAILURE\n\n";
    if (origin.seed) {
        out << "Seed:          " << *origin.seed << "\n";
        out << "Profile:       " << origin.profileName << " (generator version " << generatorVersion << ")\n";
        out << "Config:        " << origin.profileConfig << "\n";
    } else {
        out << "Stream:        " << origin.replayedFile << "\n";
    }
    out << "Command index: " << failure.commandIndex << " (0-based; " << failure.commandIndex + 1
        << " commands executed";
    if (origin.plannedCommands != 0) {
        out << " of " << origin.plannedCommands << " planned";
    }
    out << ")\n";
    out << "Command:       " << formatCommand(failure.command) << "\n";
    out << "Failure:       " << failureTypeName(failure.type) << "\n";
    if (failure.violation) {
        out << "Invariant:     " << invariantViolationName(*failure.violation) << "\n";
    }
    out << "\nReason:\n" << indent(failure.reason, "  ");
    out << "\nState after the command:\n" << failure.bookState;

    const std::size_t first = failure.commandIndex > precedingShown ? failure.commandIndex - precedingShown : 0;
    out << "\nCommands leading to it (the last " << failure.commandIndex - first << " of "
        << failure.commandIndex << " before it):\n";
    for (std::size_t i = first; i <= failure.commandIndex && i < prefix.size(); ++i) {
        out << "  " << i << "  " << formatCommand(prefix[i]) << (i == failure.commandIndex ? "   <-- failed" : "")
            << "\n";
    }

    out << "\n";
    if (!preserved.prefixPath.empty()) {
        out << "Executed prefix:  " << preserved.prefixPath << " (" << failure.commandIndex + 1 << " commands)\n";
    }
    if (preserved.shrunk) {
        const ShrinkResult& shrunk = *preserved.shrunk;
        out << "Minimized stream: " << (preserved.minimizedPath.empty() ? "(not saved)" : preserved.minimizedPath)
            << " (" << shrunk.commands.size() << " commands, after " << shrunk.attempts << " replays"
            << (shrunk.complete ? "" : "; the replay budget ran out, so it may shrink further") << ")\n";
        out << "  It fails with " << failureTypeName(shrunk.failure.type)
            << (shrunk.failure.violation ? std::string(" ") + invariantViolationName(*shrunk.failure.violation) : "")
            << " at command " << shrunk.failure.commandIndex << ":\n";
        if (shrunk.commands.size() <= minimizedShown) {
            for (std::size_t i = 0; i < shrunk.commands.size(); ++i) {
                out << "    " << i << "  " << formatCommand(shrunk.commands[i]) << "\n";
            }
        }
        out << indent(shrunk.failure.reason, "    ");
    }
    if (!preserved.problem.empty()) {
        out << "Note: " << preserved.problem << "\n";
    }

    out << "\nReplay:\n";
    if (!preserved.minimizedPath.empty()) {
        out << "  " << origin.program << " --replay " << preserved.minimizedPath << "\n";
    }
    if (!preserved.prefixPath.empty()) {
        out << "  " << origin.program << " --replay " << preserved.prefixPath << "\n";
    }
    if (origin.seed) {
        out << "  " << seedReplayCommand(origin, failure.commandIndex + 1) << "\n";
    } else {
        out << "  " << origin.program << " --replay " << origin.replayedFile << "\n";
    }
    out << "\nTo keep it as a permanent regression, copy the minimized stream into tests/randomized/regressions/,\n"
           "which every ctest run replays exactly, whatever later changes the generator.";
    if (origin.seed) {
        out << " The seed reproduces it only\nunder generator version " << generatorVersion
            << "; to also run it from the seed, add {\"" << origin.profileName << "\", " << *origin.seed << ", "
            << failure.commandIndex + 1 << "} to regressionSeeds in tests/randomized/randomized_main.cpp.";
    }
    out << "\n";
    return out.str();
}

}  // namespace order_book::randomized
