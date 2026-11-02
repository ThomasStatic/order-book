// The engine's randomized harness, one thread and no files.
//
//   engine_randomized_tests                       run the committed seeds
//   engine_randomized_tests --seed S [options]    run one seed
//   engine_randomized_tests --explore R [options] run R fresh seeds, each printed first
//
// Options: --commands N, --instruments N, --shards N, --spread uniform|skewed|one-hot, and
// --stepper run|stop, which runs the gateway stepper's schedule for each seed instead, with no
// stop or with one, on its own default instruments and shards unless those are given too.
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <exception>
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
    bool instrumentsGiven = false;
    bool shardsGiven = false;
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
           spreadName(config.spread);
}

StepperConfig stepperConfig(const Options& options, std::uint64_t seed) {
    StepperConfig config;
    config.seed = seed;
    config.commands = options.config.commands;
    config.stop = *options.stepperStop;
    if (options.instrumentsGiven) {
        config.instruments = options.config.instruments;
    }
    if (options.shardsGiven) {
        config.shards = options.config.shards;
    }
    return config;
}

std::string reproduction(const StepperConfig& config) {
    return "engine_randomized_tests --stepper " + std::string(config.stop ? "stop" : "run") + " --seed " + std::to_string(config.seed) +
           " --commands " + std::to_string(config.commands) + " --instruments " + std::to_string(config.instruments) + " --shards " +
           std::to_string(config.shards);
}

bool runSchedule(const StepperConfig& config) {
    order_book::randomized::setAbortContext("stepper seed " + std::to_string(config.seed), reproduction(config), false);
    const StepperResult result = runStepperSchedule(config);
    order_book::randomized::clearAbortContext();
    if (result.failure) {
        std::cerr << "FAILED: " << *result.failure << "\nreproduce with: " << reproduction(config) << std::endl;
        return false;
    }
    return true;
}

bool runSeed(const EngineSeedConfig& config) {
    order_book::randomized::setAbortContext("seed " + std::to_string(config.seed), reproduction(config), false);
    const EngineRunResult result = runEngineSeed(config);
    order_book::randomized::clearAbortContext();
    if (const std::optional<EngineFailure>& failure = result.failure) {
        std::cerr << "FAILED: command " << failure->commandIndex << " of instrument " << failure->instrument << ": "
                  << failure->reason << "\nreproduce with: " << reproduction(config) << std::endl;
        return false;
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
        } else {
            return "unknown option '" + std::string(arg) + "'";
        }
    }
    return std::nullopt;
}

int runStepper(const Options& options) {
    if (options.seed) {
        return runSchedule(stepperConfig(options, *options.seed)) ? EXIT_SUCCESS : EXIT_FAILURE;
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
        if (!runSchedule(config)) {
            return EXIT_FAILURE;
        }
    }
    std::cout << "explored " << *options.explore << " fresh stepper schedules without a failure" << std::endl;
    return EXIT_SUCCESS;
}

int runAll(const Options& options) {
    if (options.stepperStop) {
        return runStepper(options);
    }
    if (options.seed) {
        EngineSeedConfig config = options.config;
        config.seed = *options.seed;
        return runSeed(config) ? EXIT_SUCCESS : EXIT_FAILURE;
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
    for (const std::uint64_t seed : committedSeeds) {
        for (const Spread spread : {Spread::UNIFORM, Spread::SKEWED}) {
            EngineSeedConfig config = options.config;
            config.seed = seed;
            config.spread = spread;
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
