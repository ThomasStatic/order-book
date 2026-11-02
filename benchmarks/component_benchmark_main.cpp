#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#endif

#include "benchmark_report.hpp"
#include "component_benchmark.hpp"
#include "thread_setup.hpp"

namespace {

namespace bench = order_book::benchmark;
namespace latency = order_book::latency;

constexpr int runFailure = 1;  // an operation failed, a setting was refused, or a file failed
constexpr int usageError = 2;
constexpr int unoptimizedBuild = 3;

constexpr std::string_view usageText =
    "usage: order_book_component_benchmarks [--group capacity|controls|log|entry|wire]... [--size smoke|full]\n"
    "                                       [--threads N] [--pin-cores A,B,...] [--priority normal|high]\n"
    "                                       [--csv FILE] [--allow-unoptimized]\n"
    "       order_book_component_benchmarks --help\n";

constexpr std::string_view helpText =
    "Times one operation in bulk, on a prepared structure, and prints its mean cost. Every\n"
    "percentile comes from the latency harness, not from here.\n"
    "\n"
    "options:\n"
    "  --group NAME         capacity (growth, reserve, new levels), controls (a CPU-only loop and\n"
    "                       a memory walk at three sizes), log (encoding and reading a command log\n"
    "                       against CSV), entry (symbols, executeCommand, checkEntry), or wire (frame\n"
    "                       decode, event frame encode); repeatable (default: all)\n"
    "  --size SIZE          full (the default) or smoke, a tiny run for testing the program\n"
    "  --threads N          threads running the controls at once (default 1)\n"
    "  --pin-cores LIST     pin controls thread i to the i-th core listed\n"
    "  --priority LEVEL     normal (the default) or high, on the controls' threads\n"
    "  --csv FILE           write the results as CSV\n"
    "  --allow-unoptimized  run even in a build with assertions, automatic invariant checks, or a\n"
    "                       sanitizer; its numbers are not meaningful\n"
    "  -h, --help           print this help and exit\n"
    "\n"
    "exit status:\n"
    "  0  every operation completed\n"
    "  1  an operation failed, a setting was refused, or the file could not be written\n"
    "  2  usage error\n"
    "  3  the build is not optimized and --allow-unoptimized was not given\n";

struct Settings {
    bool capacity = false;
    bool controls = false;
    bool log = false;
    bool entry = false;
    bool wire = false;
    bool smoke = false;
    std::size_t threads = 1;
    std::vector<unsigned int> pinCores;
    bool highPriority = false;
    std::optional<std::string> csvPath;
    bool allowUnoptimized = false;
    bool help = false;
};

int usage(std::string_view problem) {
    std::cerr << "order_book_component_benchmarks: " << problem << '\n' << usageText;
    return usageError;
}

std::optional<std::uint64_t> parseNumber(std::string_view text) {
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::vector<unsigned int>> parseCores(std::string_view text) {
    std::vector<unsigned int> cores;
    while (true) {
        const std::size_t comma = text.find(',');
        const std::optional<std::uint64_t> core = parseNumber(text.substr(0, comma));
        if (!core) {
            return std::nullopt;
        }
        cores.push_back(static_cast<unsigned int>(*core));
        if (comma == std::string_view::npos) {
            return cores;
        }
        text.remove_prefix(comma + 1);
    }
}

// An error message for a usage error, or nothing once settings is filled in.
std::optional<std::string> parseArguments(const std::vector<std::string_view>& args, Settings& settings) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "-h" || arg == "--help") {
            settings.help = true;
            return std::nullopt;
        }
        if (arg == "--allow-unoptimized") {
            settings.allowUnoptimized = true;
            continue;
        }
        if (arg != "--group" && arg != "--size" && arg != "--threads" && arg != "--pin-cores" &&
            arg != "--priority" && arg != "--csv") {
            return "unknown option '" + std::string(arg) + "'";
        }
        if (i + 1 == args.size()) {
            return std::string(arg) + " needs a value";
        }
        const std::string_view value = args[++i];
        if (arg == "--group") {
            if (value == "capacity") {
                settings.capacity = true;
            } else if (value == "controls") {
                settings.controls = true;
            } else if (value == "log") {
                settings.log = true;
            } else if (value == "entry") {
                settings.entry = true;
            } else if (value == "wire") {
                settings.wire = true;
            } else {
                return "unknown group '" + std::string(value) + "' (known: capacity, controls, log, entry, wire)";
            }
        } else if (arg == "--size") {
            if (value != "full" && value != "smoke") {
                return "unknown size '" + std::string(value) + "' (known: full, smoke)";
            }
            settings.smoke = value == "smoke";
        } else if (arg == "--threads") {
            const std::optional<std::uint64_t> number = parseNumber(value);
            if (!number || *number == 0) {
                return "--threads needs a positive integer, not '" + std::string(value) + "'";
            }
            settings.threads = static_cast<std::size_t>(*number);
        } else if (arg == "--pin-cores") {
            const std::optional<std::vector<unsigned int>> cores = parseCores(value);
            if (!cores) {
                return "--pin-cores needs a comma-separated list of cores, not '" + std::string(value) + "'";
            }
            settings.pinCores = *cores;
        } else if (arg == "--priority") {
            if (value != "normal" && value != "high") {
                return "unknown priority '" + std::string(value) + "' (known: normal, high)";
            }
            settings.highPriority = value == "high";
        } else {
            settings.csvPath = std::string(value);
        }
    }
    if (!settings.capacity && !settings.controls && !settings.log && !settings.entry && !settings.wire) {
        settings.capacity = true;
        settings.controls = true;
        settings.log = true;
        settings.entry = true;
        settings.wire = true;
    }
    if (!settings.pinCores.empty() && settings.pinCores.size() != settings.threads) {
        return "--pin-cores lists " + std::to_string(settings.pinCores.size()) + " cores for " +
               std::to_string(settings.threads) + " threads";
    }
    return std::nullopt;
}

void printResult(const bench::ComponentResult& result) {
    std::cout << result.operation << ' ' << result.workload << ' ' << result.state;
    if (result.orders != 0) {
        std::cout << ", " << result.orders << " orders";
    }
    if (result.levels != 0) {
        std::cout << ", " << result.levels << " levels";
    }
    std::cout << ": " << bench::formatFixed(result.nanosecondsPerOperation(), 1) << " ns per operation over "
              << result.operations << '\n';
}

int runBenchmarks(const Settings& settings) {
    const bench::BuildInfo build = bench::currentBuild();
    if (!build.optimized() && !settings.allowUnoptimized) {
        std::cerr << "order_book_component_benchmarks: this build is not optimized (config " << build.config << ").\n"
                  << "Measure a Release build without ORDER_BOOK_INVARIANT_CHECKS=ON or sanitizers, or pass\n"
                  << "--allow-unoptimized to run anyway.\n";
        return unoptimizedBuild;
    }

    std::vector<bench::ComponentResult> results;
    if (settings.capacity) {
        bench::CapacitySettings capacity;
        if (settings.smoke) {
            capacity.orderCounts = {12};
            capacity.levelCounts = {40};
            capacity.growthPasses = 2;
            capacity.levelPasses = 2;
        }
        for (bench::ComponentResult& result : bench::runCapacityGroup(capacity)) {
            results.push_back(std::move(result));
        }
    }
    bench::WorkloadComponentSettings workloads;
    if (settings.smoke) {
        workloads.commands = 200;
        workloads.passes = 1;
        workloads.workloads = {"small-book", "quote-refresh"};
    }
    if (settings.log) {
        for (bench::ComponentResult& result : bench::runLogGroup(workloads)) {
            results.push_back(std::move(result));
        }
    }
    if (settings.entry) {
        for (bench::ComponentResult& result : bench::runEntryGroup(workloads)) {
            results.push_back(std::move(result));
        }
    }
    if (settings.wire) {
        for (bench::ComponentResult& result : bench::runWireGroup(workloads)) {
            results.push_back(std::move(result));
        }
    }
    if (settings.controls) {
        bench::ControlSettings controls;
        controls.threads = settings.threads;
        if (settings.smoke) {
            controls.walkBytes = {4096, 16384, 65536};
            controls.cpuIterations = 1000;
            controls.walkSteps = 1000;
        }
        controls.setUpThread = [&settings](std::size_t thread) {
            if (!settings.pinCores.empty()) {
                latency::pinCurrentThread(settings.pinCores[thread]);
            }
            if (settings.highPriority) {
                latency::raiseCurrentThreadPriority();
            }
        };
        for (bench::ComponentResult& result : bench::runControlGroup(controls)) {
            results.push_back(std::move(result));
        }
    }

    for (const bench::ComponentResult& result : results) {
        printResult(result);
    }
    std::cout.flush();
    if (settings.csvPath) {
        // Binary mode, so the file has LF line endings on every platform.
        std::ofstream file(*settings.csvPath, std::ios::binary);
        file << bench::formatComponentCsv(results);
        file.close();
        if (!file) {
            std::cerr << "order_book_component_benchmarks: could not write " << *settings.csvPath << '\n';
            return runFailure;
        }
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    const std::vector<std::string_view> args(argv + 1, argv + argc);
    Settings settings;
    if (const std::optional<std::string> problem = parseArguments(args, settings)) {
        return usage(*problem);
    }
    if (settings.help) {
        std::cout << usageText << '\n' << helpText;
        return EXIT_SUCCESS;
    }

    try {
        return runBenchmarks(settings);
    } catch (const std::exception& error) {
        std::cout.flush();
        std::cerr << "order_book_component_benchmarks: " << error.what() << '\n';
        return runFailure;
    }
}
