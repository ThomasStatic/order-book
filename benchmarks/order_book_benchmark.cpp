#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "benchmark_report.hpp"
#include "benchmark_runner.hpp"
#include "workload.hpp"

namespace {
    namespace bench = order_book::benchmark;

    constexpr int runFailure = 1;     // a run's outcome differed from its workload's, or a file could not be written
    constexpr int usageError = 2;
    constexpr int unoptimizedBuild = 3;

    constexpr std::string_view usageText =
        "usage: order_book_benchmarks [--workload NAME]... [--size smoke|standard|extended | --commands N]\n"
        "                             [--kind core|replay|both] [--repetitions N] [--warmup N] [--seed N]\n"
        "                             [--csv FILE] [--json FILE] [--allow-unoptimized]\n"
        "       order_book_benchmarks --list\n"
        "       order_book_benchmarks --help\n";

    constexpr std::string_view helpText =
        "Generates each workload from its configuration and seed, then times its commands on a\n"
        "freshly preloaded book, repeating every run. Generation, preloading, and checking happen\n"
        "outside the timed region, and every run's outcome is checked against the workload's own\n"
        "prediction. Prints each run, then a summary of the measured repetitions.\n"
        "\n"
        "options:\n"
        "  --workload NAME      run this built-in workload (repeatable; default: all of them)\n"
        "  --size SIZE          timed commands per run: smoke (10000), standard (100000, the\n"
        "                       default), or extended (1000000)\n"
        "  --commands N         exactly N timed commands per run, instead of a size\n"
        "  --kind KIND          core (the default) times the engine alone; replay times parsing and\n"
        "                       replaying the same commands as CSV text in memory; both runs each\n"
        "  --repetitions N      measured runs per workload and kind (default 5)\n"
        "  --warmup N           runs before those, recorded but left out of summaries (default 1)\n"
        "  --seed N             generator seed (default 42); the same seed, configuration, and\n"
        "                       generator version always give the same commands\n"
        "  --csv FILE           write every run, warm-ups included, as CSV\n"
        "  --json FILE          write the whole session (build, workloads, summaries, runs) as JSON\n"
        "  --allow-unoptimized  run even in a build with assertions, automatic invariant checks,\n"
        "                       or a sanitizer; its numbers are not meaningful\n"
        "  --list               list the built-in workloads and exit\n"
        "  -h, --help           print this help and exit\n"
        "\n"
        "exit status:\n"
        "  0  every run completed\n"
        "  1  a run did not do what its workload predicted, or an output file could not be written\n"
        "  2  usage error\n"
        "  3  the build is not optimized and --allow-unoptimized was not given\n";

    struct Settings {
        std::vector<std::string> workloads;
        std::string size = bench::benchmarkSizeName(bench::BenchmarkSize::STANDARD);
        std::size_t timedCommands = bench::timedCommandCount(bench::BenchmarkSize::STANDARD);
        std::vector<bench::BenchmarkKind> kinds{bench::BenchmarkKind::CORE};
        bench::RunSettings run;
        std::uint64_t seed = 42;
        std::optional<std::string> csvPath;
        std::optional<std::string> jsonPath;
        bool allowUnoptimized = false;
        bool list = false;
        bool help = false;
    };

    int usage(std::string_view problem) {
        std::cerr << "order_book_benchmarks: " << problem << '\n' << usageText;
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

    // An error message for a usage error, or nothing once settings is filled in.
    std::optional<std::string> parseArguments(const std::vector<std::string_view>& args, Settings& settings) {
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string_view arg = args[i];
            if (arg == "-h" || arg == "--help") {
                settings.help = true;
                return std::nullopt;
            }
            if (arg == "--list") {
                settings.list = true;
                continue;
            }
            if (arg == "--allow-unoptimized") {
                settings.allowUnoptimized = true;
                continue;
            }
            const bool takesValue = arg == "--workload" || arg == "--size" || arg == "--commands" ||
                                    arg == "--kind" || arg == "--repetitions" || arg == "--warmup" ||
                                    arg == "--seed" || arg == "--csv" || arg == "--json";
            if (!takesValue) {
                return "unknown option '" + std::string(arg) + "'";
            }
            if (i + 1 == args.size()) {
                return std::string(arg) + " needs a value";
            }
            const std::string_view value = args[++i];
            try {
                if (arg == "--workload") {
                    settings.workloads.emplace_back(bench::findWorkload(value).name);
                } else if (arg == "--size") {
                    const bench::BenchmarkSize size = bench::parseBenchmarkSize(value);
                    settings.size = bench::benchmarkSizeName(size);
                    settings.timedCommands = bench::timedCommandCount(size);
                } else if (arg == "--kind") {
                    if (value == "both") {
                        settings.kinds = {bench::BenchmarkKind::CORE, bench::BenchmarkKind::REPLAY};
                    } else {
                        settings.kinds = {bench::parseBenchmarkKind(value)};
                    }
                } else if (arg == "--csv") {
                    settings.csvPath = std::string(value);
                } else if (arg == "--json") {
                    settings.jsonPath = std::string(value);
                } else {
                    const std::optional<std::uint64_t> number = parseNumber(value);
                    if (!number) {
                        return std::string(arg) + " needs a non-negative integer, not '" + std::string(value) + "'";
                    }
                    if (arg == "--seed") {
                        settings.seed = *number;
                    } else if (arg == "--warmup") {
                        settings.run.warmups = static_cast<std::size_t>(*number);
                    } else if (*number == 0) {
                        return std::string(arg) + " must be at least 1";
                    } else if (arg == "--repetitions") {
                        settings.run.repetitions = static_cast<std::size_t>(*number);
                    } else {
                        settings.size = "custom";
                        settings.timedCommands = static_cast<std::size_t>(*number);
                    }
                }
            } catch (const std::invalid_argument& error) {
                return std::string(error.what());
            }
        }
        if (settings.workloads.empty()) {
            for (const bench::WorkloadConfig& config : bench::builtInWorkloads()) {
                settings.workloads.push_back(config.name);
            }
        }
        return std::nullopt;
    }

    void listWorkloads() {
        for (const bench::WorkloadConfig& config : bench::builtInWorkloads()) {
            std::cout << config.name << ": " << config.objective << "\n  initial orders " << config.initialOrders
                      << ", target levels " << config.targetLevels << ", cancel " << config.cancelPercent
                      << "%, prices uniform/concentrated/marketable " << config.uniformPercent << '/'
                      << config.concentratedPercent << '/' << config.marketablePercent
                      << "%, quantities small/medium/large " << config.smallPercent << '/' << config.mediumPercent
                      << '/' << config.largePercent << '%';
            if (config.activeOrderCap > 0) {
                std::cout << ", capped at " << config.activeOrderCap << " orders";
            }
            std::cout << '\n';
        }
    }

    std::string milliseconds(std::int64_t nanoseconds) {
        return bench::formatFixed(static_cast<double>(nanoseconds) / 1e6, 3) + " ms";
    }

    void printMeasurement(const bench::Measurement& measurement) {
        std::cout << "  " << bench::benchmarkKindName(measurement.kind) << ' '
                  << (measurement.warmup ? "warm-up " : "run ") << measurement.repetition << ": "
                  << measurement.commands << " commands in " << milliseconds(measurement.elapsedNanoseconds)
                  << ", " << bench::formatFixed(measurement.operationsPerSecond(), 0) << " ops/s\n";
    }

    // Writes text in binary mode, so the file has LF line endings on every platform.
    bool writeFile(const std::string& path, const std::string& text) {
        std::ofstream file(path, std::ios::binary);
        file << text;
        file.close();
        return static_cast<bool>(file);
    }

    int runBenchmarks(const Settings& settings) {
        bench::BenchmarkReport report;
        report.build = bench::currentBuild();
        if (!report.build.optimized() && !settings.allowUnoptimized) {
            std::cerr << "order_book_benchmarks: this build is not optimized (config " << report.build.config
                      << ", NDEBUG " << (report.build.ndebug ? "on" : "off") << ", invariant checks "
                      << (report.build.invariantChecks ? "on" : "off") << ", AddressSanitizer "
                      << (report.build.addressSanitizer ? "on" : "off") << ", UBSan "
                      << (report.build.undefinedBehaviorSanitizer ? "on" : "off") << ").\n"
                      << "Benchmark a Release build without ORDER_BOOK_INVARIANT_CHECKS=ON or sanitizers, or pass\n"
                      << "--allow-unoptimized to run anyway.\n";
            return unoptimizedBuild;
        }
        report.seed = settings.seed;
        report.size = settings.size;
        report.timedCommands = settings.timedCommands;
        report.settings = settings.run;
        report.startedAtUnixSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count();

        std::cout << "order_book_benchmarks: generator v" << bench::workloadGeneratorVersion << ", seed "
                  << settings.seed << ", size " << settings.size << " (" << settings.timedCommands
                  << " timed commands), " << settings.run.warmups << " warm-up and " << settings.run.repetitions
                  << " measured runs each\n"
                  << "build: " << report.build.config << ", " << report.build.compiler << '\n';
        if (!report.build.optimized()) {
            std::cout << "WARNING: unoptimized build; these numbers say nothing about the engine's speed\n";
        }

        for (const std::string& name : settings.workloads) {
            const bench::WorkloadConfig& config = bench::findWorkload(name);
            const auto generationStart = std::chrono::steady_clock::now();
            const bench::Workload workload = bench::generateWorkload(config, settings.timedCommands, settings.seed);
            const auto generationStop = std::chrono::steady_clock::now();

            bench::WorkloadRecord record;
            record.config = config;
            record.fingerprint = bench::workloadFingerprint(workload);
            record.preloadOrders = workload.preload.size();
            record.timedCommands = workload.timed.size();
            record.expected = workload.expected;
            record.generationNanoseconds =
                std::chrono::duration_cast<std::chrono::nanoseconds>(generationStop - generationStart).count();
            report.workloads.push_back(record);

            std::cout << '\n' << config.name << ": " << record.preloadOrders << " preloaded orders, "
                      << record.timedCommands << " timed commands (cancellation rate "
                      << bench::formatFixed(workload.expected.cancellationRate(), 3) << ", "
                      << workload.expected.trades << " trades, ending with " << workload.expected.finalActiveOrders
                      << " orders on " << workload.expected.finalPriceLevels << " levels); generated in "
                      << milliseconds(record.generationNanoseconds) << ", untimed\n";
            for (const bench::BenchmarkKind kind : settings.kinds) {
                for (bench::Measurement& measurement : bench::runRepeated(workload, kind, settings.run)) {
                    printMeasurement(measurement);
                    report.measurements.push_back(std::move(measurement));
                }
            }
        }

        std::cout << "\nsummary of measured runs (warm-ups excluded):\n"
                  << bench::formatSummaryTable(bench::summarize(report.measurements));
        std::cout.flush();

        if (settings.csvPath && !writeFile(*settings.csvPath, bench::formatMeasurementsCsv(report))) {
            std::cerr << "order_book_benchmarks: could not write " << *settings.csvPath << '\n';
            return runFailure;
        }
        if (settings.jsonPath && !writeFile(*settings.jsonPath, bench::formatReportJson(report))) {
            std::cerr << "order_book_benchmarks: could not write " << *settings.jsonPath << '\n';
            return runFailure;
        }
        return EXIT_SUCCESS;
    }
}

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    Settings settings;
    if (const std::optional<std::string> problem = parseArguments(args, settings)) {
        return usage(*problem);
    }
    if (settings.help) {
        std::cout << usageText << '\n' << helpText;
        return EXIT_SUCCESS;
    }
    if (settings.list) {
        listWorkloads();
        return EXIT_SUCCESS;
    }

    try {
        return runBenchmarks(settings);
    } catch (const std::exception& error) {
        std::cout.flush();
        std::cerr << "order_book_benchmarks: " << error.what() << '\n';
        return runFailure;
    }
}
