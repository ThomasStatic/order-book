// The engine's randomized harness, one thread and no files.
//
//   engine_randomized_tests                       run the committed seeds
//   engine_randomized_tests --seed S [options]    run one seed
//   engine_randomized_tests --explore R [options] run R fresh seeds, each printed first
//
// Options: --commands N, --instruments N, --shards N, --spread uniform|skewed|one-hot,
// --durability machine|process (by default, the committed seeds run both), --crashes on|off, and
// --stepper run|stop, which runs the gateway stepper's schedule for each seed instead, with no
// stop or with one, on its own default instruments and shards unless those are given too, and
// with --gateway-threads M, --connections N, --placement hashed|pinned, --stream-seed S (the
// stream's seed, where --seed is then only the schedule's), --lines I,J,... (just those lines
// of the stream, which is how a shrunk reproduction runs), and --log on|off (every shard logging,
// with no response released before its command is in the log; off by default).
//
// --threads run runs real gateway, matching, and log writer threads instead, a connection per
// instrument, with --gateway-threads M, --ring-slots R (0 for the engine's sizes), --spin-window-ns
// W, --connections-per-instrument K (K connections on each instrument, held to its log's replay),
// and --out-dir DIR, where a failure's triage is saved as well as printed.
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "abort_reporter.hpp"
#include "engine_harness.hpp"
#include "gateway_stepper.hpp"
#include "threaded_run.hpp"

namespace {

using namespace order_book::engine_testing;
using order_book::reference::Spread;

constexpr int usageError = 2;

// Run by every ctest. Each runs every built-in profile on its instruments.
constexpr std::uint64_t committedSeeds[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

struct Options {
    EngineSeedConfig config;
    std::optional<std::uint64_t> seed;
    std::optional<std::uint64_t> explore;
    std::optional<bool> stepperStop;  // set by --stepper: whether its schedules land a stop
    bool threads = false;             // set by --threads run
    bool instrumentsGiven = false;
    bool durabilityGiven = false;
    bool shardsGiven = false;
    std::size_t gatewayThreads = 1;
    std::optional<std::size_t> connections;
    bool pinned = false;
    bool log = false;
    std::optional<std::uint64_t> streamSeed;
    std::optional<std::vector<std::size_t>> lines;
    std::size_t ringSlots = 0;
    std::size_t connectionsPerInstrument = 1;
    std::chrono::nanoseconds spinWindow{0};
    std::optional<std::string> outDir;
};

std::optional<std::uint64_t> parseNumber(std::string_view text) {
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<Spread> parseSpread(std::string_view text) {
    if (text == "uniform") {
        return Spread::UNIFORM;
    }
    if (text == "skewed") {
        return Spread::SKEWED;
    }
    if (text == "one-hot") {
        return Spread::ONE_HOT;
    }
    return std::nullopt;
}

const char* spreadName(Spread spread) {
    switch (spread) {
        case Spread::UNIFORM: return "uniform";
        case Spread::SKEWED: return "skewed";
        case Spread::ONE_HOT: return "one-hot";
    }
    return "unknown";
}

std::string reproduction(const EngineSeedConfig& config) {
    return "engine_randomized_tests --seed " + std::to_string(config.seed) + " --commands " + std::to_string(config.commands) +
           " --instruments " + std::to_string(config.instruments) + " --shards " + std::to_string(config.shards) + " --spread " +
           spreadName(config.spread) + " --durability " +
           (config.durability == order_book::runtime::Durability::MACHINE ? "machine" : "process") + " --crashes " +
           (config.crashes ? "on" : "off");
}

StepperConfig stepperConfig(const Options& options, std::uint64_t seed) {
    StepperConfig config;
    config.seed = seed;
    config.streamSeed = options.streamSeed;
    config.commands = options.config.commands;
    config.stop = *options.stepperStop;
    if (options.instrumentsGiven) {
        config.instruments = options.config.instruments;
    }
    if (options.shardsGiven) {
        config.shards = options.config.shards;
    }
    config.gatewayThreads = options.gatewayThreads;
    config.connections = options.connections.value_or(config.connections);
    config.pinned = options.pinned;
    config.log = options.log;
    return config;
}

std::string reproduction(const StepperConfig& config, const std::optional<std::vector<std::size_t>>& lines) {
    std::string line = "engine_randomized_tests --stepper " + std::string(config.stop ? "stop" : "run") + " --seed " +
                       std::to_string(config.seed) + " --commands " + std::to_string(config.commands) + " --instruments " +
                       std::to_string(config.instruments) + " --shards " + std::to_string(config.shards) + " --gateway-threads " +
                       std::to_string(config.gatewayThreads) + " --connections " + std::to_string(config.connections) + " --placement " +
                       (config.pinned ? "pinned" : "hashed");
    if (config.streamSeed) {
        line += " --stream-seed " + std::to_string(*config.streamSeed);
    }
    if (config.log) {
        line += " --log on";
    }
    if (lines) {
        line += " --lines ";
        for (std::size_t i = 0; i < lines->size(); ++i) {
            line += (i == 0 ? "" : ",") + std::to_string((*lines)[i]);
        }
    }
    return line;
}

bool runSchedule(const StepperConfig& config, const std::optional<std::vector<std::size_t>>& lines) {
    order_book::randomized::setAbortContext("stepper seed " + std::to_string(config.seed), reproduction(config, lines), false);
    StepperResult result;
    if (lines) {
        EngineSeedConfig generation;
        generation.seed = config.streamSeed.value_or(config.seed);
        generation.instruments = config.instruments;
        generation.shards = config.shards;
        generation.commands = config.commands;
        generation.logWritePercent = 0;  // its log writers are actors of their own
        generation.crashes = false;
        const std::vector<EngineCommand> all = generateEngineStream(generation);
        std::vector<EngineCommand> kept;
        for (const std::size_t index : *lines) {
            if (index >= all.size()) {
                std::cerr << "FAILED: --lines names line " << index << " of a stream of " << all.size() << std::endl;
                return false;
            }
            kept.push_back(all[index]);
        }
        result = runStepperSchedule(config, kept);
    } else {
        result = runStepperSchedule(config);
    }
    order_book::randomized::clearAbortContext();
    if (result.failure) {
        std::cerr << "FAILED: " << *result.failure << "\nreproduce with: " << reproduction(config, lines) << std::endl;
        return false;
    }
    return true;
}

ThreadedConfig threadedConfig(const Options& options, std::uint64_t seed) {
    ThreadedConfig config;
    config.seed = seed;
    config.commands = options.config.commands;
    if (options.instrumentsGiven) {
        config.instruments = options.config.instruments;
    }
    if (options.shardsGiven) {
        config.shards = options.config.shards;
    }
    config.gatewayThreads = options.gatewayThreads;
    config.ringSlots = options.ringSlots;
    config.spinWindow = options.spinWindow;
    config.connectionsPerInstrument = options.connectionsPerInstrument;
    return config;
}

bool runThreaded(const ThreadedConfig& config, const std::optional<std::string>& outDir) {
    order_book::randomized::setAbortContext("real-thread seed " + std::to_string(config.seed), threadedReproduction(config), false);
    const ThreadedResult result = runThreadedSchedule(config);
    order_book::randomized::clearAbortContext();
    if (result.passed()) {
        return true;
    }
    const ThreadedTriage triage = triageThreadedFailure(config, result);
    std::cerr << "FAILED: " << triage.report << std::flush;
    if (outDir) {
        std::filesystem::create_directories(*outDir);
        const std::filesystem::path path = std::filesystem::path(*outDir) / ("threaded-" + std::to_string(config.seed) + ".txt");
        std::ofstream(path) << triage.report;
        std::cerr << "saved to " << path.string() << std::endl;
    }
    return false;
}

int runThreads(const Options& options) {
    if (options.seed) {
        return runThreaded(threadedConfig(options, *options.seed), options.outDir) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (!options.explore) {
        std::cerr << "engine_randomized_tests: --threads needs --seed or --explore" << std::endl;
        return usageError;
    }
    std::random_device device;
    std::mt19937_64 seeds((std::uint64_t{device()} << 32) | device());
    for (std::uint64_t i = 0; i < *options.explore; ++i) {
        const ThreadedConfig config = threadedConfig(options, seeds());
        std::cout << "real-thread seed " << config.seed << std::endl;
        if (!runThreaded(config, options.outDir)) {
            return EXIT_FAILURE;
        }
    }
    std::cout << "explored " << *options.explore << " fresh real-thread runs without a failure" << std::endl;
    return EXIT_SUCCESS;
}

std::optional<std::vector<std::size_t>> parseLines(std::string_view text) {
    std::vector<std::size_t> lines;
    for (;;) {
        const std::size_t comma = text.find(',');
        const std::optional<std::uint64_t> line = parseNumber(text.substr(0, comma));
        if (!line) {
            return std::nullopt;
        }
        lines.push_back(static_cast<std::size_t>(*line));
        if (comma == std::string_view::npos) {
            return lines;
        }
        text.remove_prefix(comma + 1);
    }
}

bool runSeed(const EngineSeedConfig& config, bool report = false) {
    order_book::randomized::setAbortContext("seed " + std::to_string(config.seed), reproduction(config), false);
    const EngineRunResult result = runEngineSeed(config);
    order_book::randomized::clearAbortContext();
    if (const std::optional<EngineFailure>& failure = result.failure) {
        std::cerr << "FAILED: command " << failure->commandIndex << " of instrument " << failure->instrument << ": "
                  << failure->reason << "\nreproduce with: " << reproduction(config) << std::endl;
        return false;
    }
    if (report) {
        const EngineStats& stats = result.stats;
        std::cout << "seed " << config.seed << ": queued=" << stats.queued << " crashes=" << stats.crashes
                  << " lost=" << stats.lostToCrashes << " tail-cut-bytes=" << stats.tailCutBytes << std::endl;
    }
    return true;
}

std::optional<std::string> parseArguments(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (i + 1 == argc) {
            return std::string(arg) + " needs a value";
        }
        const std::string_view value = argv[++i];
        if (arg == "--stepper") {
            if (value != "run" && value != "stop") {
                return "--stepper takes run or stop, not '" + std::string(value) + "'";
            }
            options.stepperStop = value == "stop";
            continue;
        }
        if (arg == "--threads") {
            if (value != "run") {
                return "--threads takes run, not '" + std::string(value) + "'";
            }
            options.threads = true;
            continue;
        }
        if (arg == "--placement") {
            if (value != "hashed" && value != "pinned") {
                return "--placement takes hashed or pinned, not '" + std::string(value) + "'";
            }
            options.pinned = value == "pinned";
            continue;
        }
        if (arg == "--log") {
            if (value != "on" && value != "off") {
                return "--log takes on or off, not '" + std::string(value) + "'";
            }
            options.log = value == "on";
            continue;
        }
        if (arg == "--durability") {
            if (value != "machine" && value != "process") {
                return "--durability takes machine or process, not '" + std::string(value) + "'";
            }
            options.config.durability =
                value == "machine" ? order_book::runtime::Durability::MACHINE : order_book::runtime::Durability::PROCESS;
            options.durabilityGiven = true;
            continue;
        }
        if (arg == "--crashes") {
            if (value != "on" && value != "off") {
                return "--crashes takes on or off, not '" + std::string(value) + "'";
            }
            options.config.crashes = value == "on";
            continue;
        }
        if (arg == "--out-dir") {
            options.outDir = std::string(value);
            continue;
        }
        if (arg == "--lines") {
            options.lines = parseLines(value);
            if (!options.lines) {
                return "--lines needs numbers with commas between, not '" + std::string(value) + "'";
            }
            continue;
        }
        if (arg == "--spread") {
            const std::optional<Spread> spread = parseSpread(value);
            if (!spread) {
                return "unknown spread '" + std::string(value) + "'";
            }
            options.config.spread = *spread;
            continue;
        }
        const std::optional<std::uint64_t> number = parseNumber(value);
        if (!number) {
            return std::string(arg) + " needs a number, not '" + std::string(value) + "'";
        }
        if ((arg == "--gateway-threads" || arg == "--connections" || arg == "--shards" || arg == "--instruments" ||
             arg == "--connections-per-instrument") &&
            *number == 0) {
            return std::string(arg) + " must be at least 1";
        }
        if (arg == "--seed") {
            options.seed = *number;
        } else if (arg == "--explore") {
            options.explore = *number;
        } else if (arg == "--commands") {
            options.config.commands = static_cast<std::size_t>(*number);
        } else if (arg == "--instruments") {
            options.config.instruments = static_cast<std::size_t>(*number);
            options.instrumentsGiven = true;
        } else if (arg == "--shards") {
            options.config.shards = static_cast<std::uint32_t>(*number);
            options.shardsGiven = true;
        } else if (arg == "--gateway-threads") {
            options.gatewayThreads = static_cast<std::size_t>(*number);
        } else if (arg == "--connections") {
            options.connections = static_cast<std::size_t>(*number);
        } else if (arg == "--stream-seed") {
            options.streamSeed = *number;
        } else if (arg == "--connections-per-instrument") {
            options.connectionsPerInstrument = static_cast<std::size_t>(*number);
        } else if (arg == "--ring-slots") {
            options.ringSlots = static_cast<std::size_t>(*number);
        } else if (arg == "--spin-window-ns") {
            options.spinWindow = std::chrono::nanoseconds(static_cast<std::chrono::nanoseconds::rep>(*number));
        } else {
            return "unknown option '" + std::string(arg) + "'";
        }
    }
    return std::nullopt;
}

int runStepper(const Options& options) {
    if (options.seed) {
        return runSchedule(stepperConfig(options, *options.seed), options.lines) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (!options.explore) {
        std::cerr << "engine_randomized_tests: --stepper needs --seed or --explore" << std::endl;
        return usageError;
    }
    std::random_device device;
    std::mt19937_64 seeds((std::uint64_t{device()} << 32) | device());
    for (std::uint64_t i = 0; i < *options.explore; ++i) {
        const StepperConfig config = stepperConfig(options, seeds());
        std::cout << "stepper seed " << config.seed << std::endl;
        if (!runSchedule(config, std::nullopt)) {
            return EXIT_FAILURE;
        }
    }
    std::cout << "explored " << *options.explore << " fresh stepper schedules without a failure" << std::endl;
    return EXIT_SUCCESS;
}

int runAll(const Options& options) {
    if (options.threads) {
        return runThreads(options);
    }
    if (options.stepperStop) {
        return runStepper(options);
    }
    if (options.seed) {
        EngineSeedConfig config = options.config;
        config.seed = *options.seed;
        return runSeed(config, true) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (options.explore) {
        std::random_device device;
        std::mt19937_64 seeds((std::uint64_t{device()} << 32) | device());
        for (std::uint64_t i = 0; i < *options.explore; ++i) {
            EngineSeedConfig config = options.config;
            config.seed = seeds();
            std::cout << "seed " << config.seed << std::endl;
            if (!runSeed(config)) {
                return EXIT_FAILURE;
            }
        }
        std::cout << "explored " << *options.explore << " fresh seeds without a failure" << std::endl;
        return EXIT_SUCCESS;
    }
    // Each seed at both durabilities, one spread each, unless one was asked for.
    for (const std::uint64_t seed : committedSeeds) {
        for (const Spread spread : {Spread::UNIFORM, Spread::SKEWED}) {
            EngineSeedConfig config = options.config;
            config.seed = seed;
            config.spread = spread;
            if (!options.durabilityGiven) {
                config.durability = (spread == Spread::UNIFORM) == (seed % 2 == 0) ? order_book::runtime::Durability::MACHINE
                                                                                    : order_book::runtime::Durability::PROCESS;
            }
            if (!runSeed(config)) {
                return EXIT_FAILURE;
            }
        }
    }
    std::cout << "engine seeds passed" << std::endl;
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    order_book::randomized::installAbortReporter();
    Options options;
    if (const std::optional<std::string> problem = parseArguments(argc, argv, options)) {
        std::cerr << "engine_randomized_tests: " << *problem << std::endl;
        return usageError;
    }
    try {
        return runAll(options);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
}
