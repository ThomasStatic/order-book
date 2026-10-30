// Randomized command testing for the order book: seeded, deterministic streams of
// submissions and cancellations, valid and intentionally invalid, run against the book with
// every command checked against its invariants and an independent reference model. This is
// pseudorandom regression and exploration testing, not coverage-guided fuzzing.
//
//   randomized_tests                          run the committed regression seeds
//   randomized_tests --seed S [--profile P|all] [--commands N]
//   randomized_tests --explore R [--profile P|all] [--commands N]   R fresh seeds, each printed first
//   randomized_tests --replay FILE            run a saved stream exactly
//   randomized_tests --regressions DIR        replay every saved stream (*.txt) in DIR
//   randomized_tests --seed S --profile P [--commands N] --write FILE   save a stream without running it
//   randomized_tests --list-profiles
//
// Options: --out-dir DIR (where failing streams are saved; default randomized-failures),
// --no-shrink, --shrink-budget N (most candidate replays when minimizing; default 20000).

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "abort_reporter.hpp"
#include "command.hpp"
#include "command_generator.hpp"
#include "command_replayer.hpp"
#include "failure_report.hpp"
#include "order_book/order_book.hpp"
#include "shrink.hpp"

namespace {

using namespace order_book::randomized;
namespace fs = std::filesystem;

// Run by every ctest: each profile under two fixed seeds. When exploration finds a failing
// seed, add it here too, but the durable regression is its saved stream in
// tests/randomized/regressions/: a seed reproduces its stream only under the generator
// version it was found with (generatorVersion, currently 2). The passive profile's deep book
// makes each command's checks slower, so its seeds run fewer commands.
struct RegressionSeed {
    const char* profile;
    std::uint64_t seed;
    std::size_t commands;
};

constexpr RegressionSeed regressionSeeds[] = {
    {"balanced", 1, 5000},          {"balanced", 20261022, 5000},   {"aggressive", 2, 5000},
    {"aggressive", 314159, 5000},   {"passive", 3, 3000},           {"passive", 271828, 3000},
    {"cancel-heavy", 4, 5000},      {"cancel-heavy", 161803, 5000}, {"queue", 5, 5000},
    {"queue", 141421, 5000},        {"one-sided", 6, 5000},         {"one-sided", 173205, 5000},
    {"invalid-heavy", 7, 5000},     {"invalid-heavy", 223606, 5000}, {"limits", 8, 5000},
    {"limits", 244948, 5000},
};

constexpr int exitPassed = EXIT_SUCCESS;
constexpr int exitFailed = 1;
constexpr int exitUsage = 2;

struct Options {
    std::string program;
    std::optional<std::uint64_t> seed;
    std::string profile;  // empty means the mode's default
    std::optional<std::size_t> commands;
    std::optional<std::size_t> explore;
    std::string replayFile;
    std::string regressionsDir;
    std::string writeFile;
    std::string outDir = "randomized-failures";
    bool shrink = true;
    std::size_t shrinkBudget = 20000;
    bool listProfiles = false;
};

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::uint64_t parseCount(const std::string& text, const std::string& option) {
    std::uint64_t value = 0;
    const char* end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(text.data(), end, value);
    if (text.empty() || error != std::errc{} || ptr != end) {
        throw UsageError(option + " needs a non-negative integer, got '" + text + "'");
    }
    return value;
}

Options parseOptions(int argc, char** argv) {
    Options options;
    options.program = argc > 0 ? argv[0] : "randomized_tests";
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw UsageError(option + " needs a value");
            }
            return argv[++i];
        };
        if (option == "--seed") {
            options.seed = parseCount(value(), option);
        } else if (option == "--profile") {
            options.profile = value();
        } else if (option == "--commands") {
            options.commands = static_cast<std::size_t>(parseCount(value(), option));
        } else if (option == "--explore") {
            options.explore = static_cast<std::size_t>(parseCount(value(), option));
        } else if (option == "--replay") {
            options.replayFile = value();
        } else if (option == "--regressions") {
            options.regressionsDir = value();
        } else if (option == "--write") {
            options.writeFile = value();
        } else if (option == "--out-dir") {
            options.outDir = value();
        } else if (option == "--no-shrink") {
            options.shrink = false;
        } else if (option == "--shrink-budget") {
            options.shrinkBudget = static_cast<std::size_t>(parseCount(value(), option));
        } else if (option == "--list-profiles") {
            options.listProfiles = true;
        } else {
            throw UsageError("unknown option " + option);
        }
    }
    return options;
}

std::vector<const WorkloadProfile*> selectProfiles(const std::string& name, const std::string& fallback) {
    const std::string chosen = name.empty() ? fallback : name;
    std::vector<const WorkloadProfile*> selected;
    if (chosen == "all") {
        for (const WorkloadProfile& profile : workloadProfiles()) {
            selected.push_back(&profile);
        }
    } else {
        selected.push_back(&findProfile(chosen));
    }
    return selected;
}

// Saves the executed prefix, minimizes it, saves that too, and prints the report.
void reportFailure(const Options& options, const RunOrigin& origin, const Failure& failure,
                   const std::vector<Command>& prefix, StreamHeader header, const CommandReplayer& replayer) {
    PreservedStreams preserved;
    std::error_code error;
    fs::create_directories(options.outDir, error);
    const std::string stem = origin.seed ? origin.profileName + "-seed" + std::to_string(*origin.seed)
                                         : fs::path(origin.replayedFile).stem().string();
    const std::string outcome = std::string(failureTypeName(failure.type)) +
                                (failure.violation ? std::string(" ") + order_book::invariantViolationName(*failure.violation) : "") +
                                " at command index " + std::to_string(failure.commandIndex);

    try {
        preserved.prefixPath = fs::absolute(fs::path(options.outDir) / (stem + "-prefix.txt")).string();
        header.note = "the executed prefix, ending at the first failure: " + outcome;
        saveStream(preserved.prefixPath, CommandStream{header, prefix});
    } catch (const std::exception& saveError) {
        preserved.problem = std::string("could not save the executed prefix: ") + saveError.what();
        preserved.prefixPath.clear();
    }

    if (options.shrink) {
        setAbortContext("the minimizer, replaying a candidate cut from the failing stream",
                        options.program + " --replay " +
                            (preserved.prefixPath.empty() ? origin.replayedFile : preserved.prefixPath),
                        false);
        preserved.shrunk = shrinkFailingStream(prefix, failure, replayer, options.shrinkBudget);
        clearAbortContext();
        try {
            preserved.minimizedPath = fs::absolute(fs::path(options.outDir) / (stem + "-minimized.txt")).string();
            header.note = "minimized by " + std::to_string(preserved.shrunk->attempts) +
                          " replays from a stream failing with " + outcome;
            saveStream(preserved.minimizedPath, CommandStream{header, preserved.shrunk->commands});
        } catch (const std::exception& saveError) {
            preserved.problem += std::string(preserved.problem.empty() ? "" : "; ") +
                                 "could not save the minimized stream: " + saveError.what();
            preserved.minimizedPath.clear();
        }
    }

    std::cerr << "\n" << formatFailureReport(origin, failure, prefix, preserved) << std::flush;
}

bool runSeed(const Options& options, const WorkloadProfile& profile, std::uint64_t seed, std::size_t count,
             const CommandReplayer& replayer) {
    // Printed before the run starts, so the seed is on record even if the run crashes or hangs.
    std::cout << "[run]  profile=" << profile.name << " seed=" << seed << " commands=" << count
              << " generator=" << generatorVersion << std::endl;

    RunOrigin origin{options.program, seed, profile.name, describeProfile(profile), count, ""};
    const StreamHeader header{generatorVersion, profile.name, seed, describeProfile(profile), ""};
    setAbortContext("profile " + profile.name + " seed " + std::to_string(seed) + " (generator version " +
                        std::to_string(generatorVersion) + ")",
                    options.program + " --seed " + std::to_string(seed) + " --profile " + profile.name +
                        " --commands ",
                    true);
    CommandRecorder recorder(header);
    order_book::OrderBook book;
    RandomCommandGenerator generator(seed, profile);
    const RunResult result = replayer.run(book, generator, count, recorder);
    clearAbortContext();

    if (result.passed()) {
        std::cout << "[pass] " << result.stats.summary() << std::endl;
        return true;
    }
    reportFailure(options, origin, *result.failure, recorder.commands(), header, replayer);
    return false;
}

bool replayFile(const Options& options, const std::string& path, const CommandReplayer& replayer) {
    std::cout << "[replay] " << path << std::endl;
    const CommandStream stream = loadStream(path);
    if (!stream.header.note.empty()) {
        std::cout << "         " << stream.header.note << std::endl;
    }

    RunOrigin origin{options.program, std::nullopt, stream.header.profile, stream.header.config,
                     stream.commands.size(), path};
    setAbortContext("the saved stream " + path, options.program + " --replay " + path, false);
    order_book::OrderBook book;
    const RunResult result = replayer.replay(book, stream.commands);
    clearAbortContext();

    if (result.passed()) {
        std::cout << "[pass] " << result.stats.summary() << std::endl;
        return true;
    }
    const std::vector<Command> prefix(stream.commands.begin(),
                                      stream.commands.begin() +
                                          static_cast<std::ptrdiff_t>(result.failure->commandIndex + 1));
    reportFailure(options, origin, *result.failure, prefix, stream.header, replayer);
    return false;
}

bool replayDirectory(const Options& options, const CommandReplayer& replayer) {
    if (!fs::is_directory(options.regressionsDir)) {
        throw UsageError("no such directory: " + options.regressionsDir);
    }
    std::vector<fs::path> streams;
    for (const fs::directory_entry& entry : fs::directory_iterator(options.regressionsDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".txt") {
            streams.push_back(entry.path());
        }
    }
    std::sort(streams.begin(), streams.end());
    if (streams.empty()) {
        std::cout << "no saved regression streams (*.txt) in " << options.regressionsDir << std::endl;
    }
    for (const fs::path& stream : streams) {
        if (!replayFile(options, stream.string(), replayer)) {
            return false;
        }
    }
    return true;
}

int run(const Options& options) {
    const CommandReplayer replayer;

    if (options.listProfiles) {
        for (const WorkloadProfile& profile : workloadProfiles()) {
            std::cout << profile.name << ": " << profile.purpose << "\n    " << describeProfile(profile)
                      << "\n    default commands: " << profile.defaultCommands << "\n";
        }
        return exitPassed;
    }
    if (!options.replayFile.empty()) {
        return replayFile(options, options.replayFile, replayer) ? exitPassed : exitFailed;
    }
    if (!options.regressionsDir.empty()) {
        return replayDirectory(options, replayer) ? exitPassed : exitFailed;
    }
    if (!options.writeFile.empty()) {
        if (!options.seed || options.profile.empty() || options.profile == "all") {
            throw UsageError("--write needs --seed and a single --profile");
        }
        const WorkloadProfile& profile = findProfile(options.profile);
        const std::size_t count = options.commands.value_or(profile.defaultCommands);
        const StreamHeader header{generatorVersion, profile.name, *options.seed, describeProfile(profile),
                                  "generated without running the book"};
        saveStream(options.writeFile, CommandStream{header, generateCommands(*options.seed, profile, count)});
        std::cout << "wrote " << count << " commands to " << options.writeFile << std::endl;
        return exitPassed;
    }
    if (options.explore) {
        const std::vector<const WorkloadProfile*> profiles = selectProfiles(options.profile, "all");
        std::random_device device;
        for (std::size_t i = 0; i < *options.explore; ++i) {
            const std::uint64_t seed = (std::uint64_t{device()} << 32) ^ device();
            const WorkloadProfile& profile = *profiles[i % profiles.size()];
            if (!runSeed(options, profile, seed, options.commands.value_or(profile.defaultCommands), replayer)) {
                return exitFailed;
            }
        }
        std::cout << "explored " << *options.explore << " fresh seeds without a failure" << std::endl;
        return exitPassed;
    }
    if (options.seed) {
        for (const WorkloadProfile* profile : selectProfiles(options.profile, "balanced")) {
            if (!runSeed(options, *profile, *options.seed, options.commands.value_or(profile->defaultCommands),
                         replayer)) {
                return exitFailed;
            }
        }
        return exitPassed;
    }

    for (const RegressionSeed& regression : regressionSeeds) {
        const WorkloadProfile& profile = findProfile(regression.profile);
        if (!options.profile.empty() && options.profile != "all" && options.profile != profile.name) {
            continue;
        }
        if (!runSeed(options, profile, regression.seed, options.commands.value_or(regression.commands), replayer)) {
            return exitFailed;
        }
    }
    std::cout << "randomized command tests passed" << std::endl;
    return exitPassed;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _MSC_VER
    // If the engine aborts, the reporter prints the context; skip the debug CRT's message box.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    installAbortReporter();
    try {
        return run(parseOptions(argc, argv));
    } catch (const UsageError& error) {
        std::cerr << "usage error: " << error.what() << "\n(see the comment at the top of randomized_main.cpp)"
                  << std::endl;
        return exitUsage;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << std::endl;
        return exitUsage;
    }
}
