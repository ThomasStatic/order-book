#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#endif

#include "benchmark_report.hpp"
#include "benchmark_runner.hpp"
#include "engine_process.hpp"
#include "scaling_run.hpp"
#include "workload.hpp"

namespace {
    namespace bench = order_book::benchmark;

    constexpr int runFailure = 1;     // a run's outcome differed from its workload's, or a file could not be written
    constexpr int usageError = 2;
    constexpr int unoptimizedBuild = 3;

    constexpr std::string_view usageText =
        "usage: order_book_benchmarks [--workload NAME]... [--size smoke|standard|extended|long | --commands N]\n"
        "                             [--kind core|replay|replay-log|shard|queue|gateway|engine|both] [--instruments N]\n"
        "                             [--window N] [--shards N] [--producers M] [--gateway-threads M]\n"
        "                             [--spread uniform|skewed] [--placement even|hash|hot-alone]\n"
        "                             [--pin-cores LIST] [--priority normal|high] [--layout NAME]\n"
        "                             [--frame-budget N|unlimited] [--spin-window TIME|unlimited]\n"
        "                             [--engine PATH] [--durability none|process|machine] [--log-files extended|zero-filled]\n"
        "                             [--roll-worker on|off]\n"
        "                             [--log-dir DIR] [--repetitions N] [--warmup N] [--seed N]\n"
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
        "  --workload NAME      run this workload (repeatable; default: all of them, the five v1\n"
        "                       workloads and the behavior workloads), or for the threaded kinds\n"
        "                       mixed: instruments cycling through two each of deep-book,\n"
        "                       pending-stops, immediate, and quote-refresh\n"
        "  --size SIZE          timed commands per run, and per shard for the threaded kinds: smoke\n"
        "                       (10000), standard (100000, the default), extended (1000000), or\n"
        "                       long (20000000)\n"
        "  --commands N         exactly N timed commands per run, instead of a size\n"
        "  --kind KIND          core (the default) times the engine alone; replay times parsing and\n"
        "                       replaying the same commands as CSV text in memory; replay-log the\n"
        "                       same as a binary command log; shard through Shard::process on one\n"
        "                       thread; queue with real rings and a matching thread; gateway over\n"
        "                       real loopback sockets, one connection per instrument; engine the\n"
        "                       same client against the shipped order_book_engine as a child\n"
        "                       process, at process durability; both runs core and replay\n"
        "  --instruments N      for shard, queue, gateway, and engine: share the commands between N\n"
        "                       instruments (default 1)\n"
        "  --window N           for gateway and engine: commands each connection may have unanswered\n"
        "                       (default 256)\n"
        "  --shards N           for shard, queue, gateway, and engine: N shards, a matching thread\n"
        "                       each, placed by --placement (engine: instrument i on shard i modulo N)\n"
        "                       (default 1)\n"
        "  --producers M        for queue: M producer threads, shard i fed by producer i modulo M\n"
        "                       (default 1, at most N)\n"
        "  --gateway-threads M  for gateway: M gateway threads sharing the connections as the\n"
        "                       engine's do, and M client threads, instrument k on client k modulo M\n"
        "                       (default 1)\n"
        "  --spread SPREAD      uniform (the default), every instrument an equal share of the\n"
        "                       commands, or skewed, Zipf with exponent 1, instrument 0 the top\n"
        "  --placement HOW      even (the default), instrument i on shard i modulo N; hash, by the\n"
        "                       symbol's hash, as the engine places an unpinned instrument; or\n"
        "                       hot-alone, instrument 0 alone on shard 0 and the rest even\n"
        "  --pin-cores LIST     for shard, queue, and gateway: comma-separated logical processors for\n"
        "                       the N matching threads, then the M producers or gateway threads,\n"
        "                       then gateway's M client threads, then optionally the N log writers;\n"
        "                       a later list replaces an earlier one\n"
        "  --priority LEVEL     normal (the default) or high, for every thread a threaded run places\n"
        "  --layout NAME        the name the results give the thread layout (default: pinned with\n"
        "                       --pin-cores, else unpinned)\n"
        "  --frame-budget N     for gateway: frames read from one connection a pass (default 64, the\n"
        "                       engine's), or unlimited\n"
        "  --spin-window TIME   for queue and gateway: how long the matching and gateway threads look\n"
        "                       before they sleep, as the engine's --spin-window (default 100us)\n"
        "  --engine PATH        for engine: the order_book_engine to run (default: the one beside\n"
        "                       this program)\n"
        "  --durability LEVEL   for queue and gateway: none (the default), a shard with no log;\n"
        "                       process, written to the system and flushed when a file closes; or\n"
        "                       machine, written through to the drive. engine always runs at process,\n"
        "                       and takes --durability process, so it pairs with gateway at that level\n"
        "  --log-files FILL     for queue and gateway with a log: extended (the default), as the engine\n"
        "                       makes them, or zero-filled, every byte written as zeros first\n"
        "  --roll-worker WHICH  for queue and gateway with a log: on, a roll-forward worker per shard\n"
        "                       beside its log writer, as the engine runs them, or off (the default)\n"
        "  --log-dir DIR        where each logged run, the engine's included, makes a fresh directory\n"
        "                       for its log, deleted afterwards (default: the system's temporary\n"
        "                       directory)\n"
        "  --tamper-prediction  for shard, queue, gateway, and engine: predict one trade too many, so\n"
        "                       the run must fail\n"
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
        std::size_t instruments = 1;
        bool mixed = false;  // --workload mixed
        bench::ScalingSettings scaling;
        std::optional<std::string> layout;
        bool producersGiven = false;
        bool gatewayThreadsGiven = false;
        bool spreadGiven = false;
        bool scalingGiven = false;  // any option only the threaded kinds take
        bool tamperPrediction = false;
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

    // Digits and a unit (ns, us, ms, or s), a bare 0, or unlimited, as the engine's --spin-window.
    std::optional<std::chrono::nanoseconds> parseSpinWindow(std::string_view text) {
        if (text == "unlimited") {
            return order_book::runtime::unlimitedSpin;
        }
        if (text == "0") {
            return std::chrono::nanoseconds::zero();
        }
        constexpr std::pair<std::string_view, std::uint64_t> units[] = {{"ns", 1}, {"us", 1'000}, {"ms", 1'000'000}, {"s", 1'000'000'000}};
        for (const auto& [unit, nanoseconds] : units) {
            if (text.size() > unit.size() && text.ends_with(unit)) {
                const std::string_view digits = text.substr(0, text.size() - unit.size());
                if (digits.back() < '0' || digits.back() > '9') {
                    continue;
                }
                const std::optional<std::uint64_t> count = parseNumber(digits);
                if (!count || *count > 1'000'000'000'000u / nanoseconds) {
                    return std::nullopt;
                }
                return std::chrono::nanoseconds(static_cast<std::int64_t>(*count * nanoseconds));
            }
        }
        return std::nullopt;
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
            if (arg == "--tamper-prediction") {
                settings.tamperPrediction = true;
                continue;
            }
            if (arg == "--allow-unoptimized") {
                settings.allowUnoptimized = true;
                continue;
            }
            const bool takesValue = arg == "--workload" || arg == "--size" || arg == "--commands" ||
                                    arg == "--kind" || arg == "--repetitions" || arg == "--warmup" ||
                                    arg == "--seed" || arg == "--csv" || arg == "--json" || arg == "--instruments" ||
                                    arg == "--window" || arg == "--shards" || arg == "--engine" || arg == "--durability" ||
                                    arg == "--log-dir" || arg == "--producers" || arg == "--gateway-threads" ||
                                    arg == "--spread" || arg == "--placement" || arg == "--pin-cores" || arg == "--priority" ||
                                    arg == "--layout" || arg == "--frame-budget" || arg == "--spin-window" ||
                                    arg == "--log-files" || arg == "--roll-worker";
            if (!takesValue) {
                return "unknown option '" + std::string(arg) + "'";
            }
            if (i + 1 == args.size()) {
                return std::string(arg) + " needs a value";
            }
            const std::string_view value = args[++i];
            try {
                if (arg == "--workload" && value == "mixed") {
                    settings.mixed = true;
                    settings.workloads.emplace_back("mixed");
                } else if (arg == "--workload") {
                    settings.workloads.emplace_back(bench::findWorkload(value).name);
                } else if (arg == "--spread") {
                    settings.scaling.spread = bench::parseSpread(value);
                    settings.spreadGiven = true;
                } else if (arg == "--placement") {
                    settings.scaling.placement = bench::parsePlacement(value);
                    settings.scalingGiven = true;
                } else if (arg == "--layout") {
                    settings.layout = std::string(value);
                    settings.scalingGiven = true;
                } else if (arg == "--roll-worker") {
                    if (value != "on" && value != "off") {
                        return "unknown --roll-worker '" + std::string(value) + "' (known: on, off)";
                    }
                    settings.scaling.rollWorker = value == "on";
                    settings.scalingGiven = true;
                } else if (arg == "--log-files") {
                    settings.scaling.fill = bench::parseLogFileFill(value);
                    settings.scalingGiven = true;
                } else if (arg == "--priority") {
                    if (value != "normal" && value != "high") {
                        return "unknown priority '" + std::string(value) + "' (known: normal, high)";
                    }
                    settings.scaling.plan.highPriority = value == "high";
                    settings.scalingGiven = true;
                } else if (arg == "--pin-cores") {
                    settings.scaling.plan.cores.clear();  // a later list replaces an earlier one
                    std::string_view rest = value;
                    while (!rest.empty()) {
                        const std::size_t comma = rest.find(',');
                        const std::optional<std::uint64_t> core = parseNumber(rest.substr(0, comma));
                        if (!core || *core > 1023) {
                            return "--pin-cores needs logical processor numbers separated by commas, not '" + std::string(value) + "'";
                        }
                        settings.scaling.plan.cores.push_back(static_cast<unsigned int>(*core));
                        rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
                    }
                    settings.scalingGiven = true;
                } else if (arg == "--frame-budget") {
                    if (value == "unlimited") {
                        settings.scaling.framesPerPass = std::numeric_limits<std::size_t>::max();
                    } else {
                        const std::optional<std::uint64_t> frames = parseNumber(value);
                        if (!frames || *frames == 0) {
                            return "--frame-budget needs a positive integer or unlimited, not '" + std::string(value) + "'";
                        }
                        settings.scaling.framesPerPass = static_cast<std::size_t>(*frames);
                    }
                    settings.scalingGiven = true;
                } else if (arg == "--spin-window") {
                    const std::optional<std::chrono::nanoseconds> window = parseSpinWindow(value);
                    if (!window) {
                        return "--spin-window needs a time such as 0, 20us, or 1ms, or unlimited, not '" + std::string(value) + "'";
                    }
                    settings.scaling.spinWindow = *window;
                    settings.scalingGiven = true;
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
                } else if (arg == "--engine") {
                    settings.run.engine = std::filesystem::path(value);
                } else if (arg == "--durability") {
                    settings.run.durability = bench::parseDurability(value);
                } else if (arg == "--log-dir") {
                    settings.run.logParent = std::filesystem::path(value);
                } else {
                    const std::optional<std::uint64_t> number = parseNumber(value);
                    if (!number) {
                        return std::string(arg) + " needs a non-negative integer, not '" + std::string(value) + "'";
                    }
                    if (arg == "--instruments") {
                        if (*number == 0) {
                            return "--instruments needs a positive integer";
                        }
                        settings.instruments = static_cast<std::size_t>(*number);
                    } else if (arg == "--seed") {
                        settings.seed = *number;
                    } else if (arg == "--warmup") {
                        settings.run.warmups = static_cast<std::size_t>(*number);
                    } else if (*number == 0) {
                        return std::string(arg) + " must be at least 1";
                    } else if (arg == "--repetitions") {
                        settings.run.repetitions = static_cast<std::size_t>(*number);
                    } else if (arg == "--window") {
                        settings.run.window = static_cast<std::size_t>(*number);
                    } else if (arg == "--shards") {
                        settings.run.shards = static_cast<std::size_t>(*number);
                        settings.scaling.shards = settings.run.shards;
                    } else if (arg == "--producers" || arg == "--gateway-threads") {
                        settings.scaling.threads = static_cast<std::size_t>(*number);
                        settings.scalingGiven = true;
                        (arg == "--producers" ? settings.producersGiven : settings.gatewayThreadsGiven) = true;
                    } else {
                        settings.size = "custom";
                        settings.timedCommands = static_cast<std::size_t>(*number);
                    }
                }
            } catch (const std::invalid_argument& error) {
                return std::string(error.what());
            }
        }
        if (settings.run.durability != bench::RunDurability::NONE) {
            for (const bench::BenchmarkKind kind : settings.kinds) {
                // engine takes process, its one level, so it pairs with gateway at the same level.
                const bool engineAtProcess = kind == bench::BenchmarkKind::ENGINE && settings.run.durability == bench::RunDurability::PROCESS;
                if (kind != bench::BenchmarkKind::QUEUE && kind != bench::BenchmarkKind::GATEWAY && !engineAtProcess) {
                    return "--durability is for queue and gateway, and engine always runs at process, so takes only that";
                }
            }
        }
        const bool onlyThreaded = std::all_of(settings.kinds.begin(), settings.kinds.end(), [](bench::BenchmarkKind kind) {
            return kind == bench::BenchmarkKind::SHARD || kind == bench::BenchmarkKind::QUEUE || kind == bench::BenchmarkKind::GATEWAY;
        });
        if (settings.scalingGiven && !onlyThreaded) {
            return "--producers, --gateway-threads, --placement, --pin-cores, --priority, --layout, --frame-budget, --spin-window, "
                   "--log-files, and --roll-worker are for shard, queue, and gateway";
        }
        if (settings.mixed && !std::all_of(settings.kinds.begin(), settings.kinds.end(), bench::sharesInstruments)) {
            return "--workload mixed is for shard, queue, gateway, and engine";
        }
        if (settings.mixed && settings.spreadGiven) {
            return "--workload mixed shares its commands evenly, so takes no --spread";
        }
        if (settings.spreadGiven && !std::all_of(settings.kinds.begin(), settings.kinds.end(), bench::sharesInstruments)) {
            return "--spread is for shard, queue, gateway, and engine";
        }
        const auto only = [&settings](bench::BenchmarkKind kind) {
            return std::all_of(settings.kinds.begin(), settings.kinds.end(), [kind](bench::BenchmarkKind given) { return given == kind; });
        };
        if (settings.producersGiven && (settings.gatewayThreadsGiven || !only(bench::BenchmarkKind::QUEUE))) {
            return "--producers is for queue";
        }
        if (settings.gatewayThreadsGiven && !only(bench::BenchmarkKind::GATEWAY)) {
            return "--gateway-threads is for gateway";
        }
        if (settings.run.shards > 1 && !std::all_of(settings.kinds.begin(), settings.kinds.end(), bench::sharesInstruments)) {
            return "--shards is for shard, queue, gateway, and engine";
        }
        settings.scaling.durability = settings.run.durability;
        settings.scaling.logParent = settings.run.logParent;
        settings.scaling.window = settings.run.window;
        settings.scaling.plan.layout = settings.layout ? *settings.layout : settings.scaling.plan.cores.empty() ? "unpinned" : "pinned";
        if (settings.workloads.empty()) {
            for (const auto* family : {&bench::builtInWorkloads(), &bench::behaviorWorkloads()}) {
                for (const bench::WorkloadConfig& config : *family) {
                    settings.workloads.push_back(config.name);
                }
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
        for (const bench::WorkloadConfig& config : bench::behaviorWorkloads()) {
            const bench::BehaviorMix& mix = config.behavior;
            std::cout << config.name << ": " << config.objective << "\n  ";
            if (!mix.replaysWorkload.empty()) {
                std::cout << mix.replaysWorkload << "'s commands through submitOrder(OrderRequest), "
                          << (mix.ownerPerOrder ? "a distinct owner per order" : "owner 0") << '\n';
                continue;
            }
            std::cout << "initial orders " << config.initialOrders << " (+" << mix.preloadStops << " stops), target levels "
                      << config.targetLevels << ", submit/replace/cancel/stop/cancel-stop " << mix.submitPercent << '/'
                      << mix.replacePercent << '/' << mix.cancelPercent << '/' << mix.stopPercent << '/'
                      << mix.cancelStopPercent << "%, submissions passive/marketable/IOC/FOK/market/sweep "
                      << mix.passivePercent << '/' << mix.marketablePercent << '/' << mix.iocPercent << '/'
                      << mix.fokPercent << '/' << mix.marketPercent << '/' << mix.sweepPercent << "%\n";
        }
    }

    std::string milliseconds(std::int64_t nanoseconds) {
        return bench::formatFixed(static_cast<double>(nanoseconds) / 1e6, 3) + " ms";
    }

    void printMeasurement(const bench::Measurement& measurement) {
        std::cout << "  " << bench::benchmarkKindName(measurement.kind) << ' '
                  << (measurement.warmup ? "warm-up " : "run ") << measurement.repetition << ": "
                  << measurement.commands << " commands in " << milliseconds(measurement.elapsedNanoseconds)
                  << ", " << bench::formatFixed(measurement.operationsPerSecond(), 0) << " ops/s";
        if (measurement.outcome.stopsTriggered > 0) {
            std::cout << ", " << measurement.outcome.stopsTriggered << " stops triggered";
        }
        std::cout << '\n';
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
                  << " timed commands, per shard for the threaded kinds), " << settings.run.warmups << " warm-up and "
                  << settings.run.repetitions
                  << " measured runs each\n"
                  << "build: " << report.build.config << ", " << report.build.compiler << '\n';
        if (!report.build.optimized()) {
            std::cout << "WARNING: unoptimized build; these numbers say nothing about the engine's speed\n";
        }

        for (const std::string& name : settings.workloads) {
            // Per shard for the threaded kinds, so scaling holds each shard's work fixed.
            const std::size_t threadedCommands = settings.timedCommands * settings.run.shards;
            const std::size_t instruments = std::max(settings.instruments, std::size_t{1});
            if (name == "mixed") {
                std::cout << "\nmixed: " << instruments << " instruments cycling through deep-book, pending-stops, immediate, and "
                          << "quote-refresh, " << threadedCommands << " timed commands between them\n";
                for (const bench::BenchmarkKind kind : settings.kinds) {
                    std::vector<bench::Workload> workloads =
                        bench::mixedWorkloads(threadedCommands, settings.seed, instruments, settings.run.shards);
                    for (bench::Workload& workload : workloads) {
                        workload.config.name = "mixed";
                    }
                    if (settings.tamperPrediction) {
                        workloads.front().expected.trades += 1;
                    }
                    std::vector<bench::Measurement> measurements = kind == bench::BenchmarkKind::ENGINE
                                                                       ? bench::runInstrumentsRepeated(workloads, kind, settings.run)
                                                                       : bench::runScaledRepeated(workloads, kind, settings.scaling, settings.run);
                    for (bench::Measurement& measurement : measurements) {
                        printMeasurement(measurement);
                        report.measurements.push_back(std::move(measurement));
                    }
                }
                continue;
            }
            const bench::WorkloadConfig& config = bench::findWorkload(name);
            const auto generationStart = std::chrono::steady_clock::now();
            const bench::Workload workload = bench::generateWorkload(config, settings.timedCommands, settings.seed);
            const auto generationStop = std::chrono::steady_clock::now();

            bench::WorkloadRecord record;
            record.config = config;
            record.fingerprint = bench::workloadFingerprint(workload);
            record.preloadOrders = workload.preload.size() + workload.behaviorPreload.size();
            record.timedCommands = workload.timedCount();
            const order_book::BookCapacity capacity = bench::expectedBookSize(workload);
            record.reservedOrders = capacity.orders;
            record.reservedStops = capacity.stops;
            record.reservedLevels = capacity.levels;
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
                std::vector<bench::Measurement> measurements;
                if (bench::sharesInstruments(kind)) {
                    std::vector<bench::Workload> workloads =
                        bench::spreadWorkloads(config, threadedCommands, settings.seed, instruments, settings.scaling.spread);
                    if (settings.tamperPrediction) {
                        workloads.front().expected.trades += 1;
                    }
                    measurements = kind == bench::BenchmarkKind::ENGINE
                                       ? bench::runInstrumentsRepeated(workloads, kind, settings.run)
                                       : bench::runScaledRepeated(workloads, kind, settings.scaling, settings.run);
                    for (bench::Measurement& measurement : measurements) {
                        measurement.spread = bench::spreadName(settings.scaling.spread);  // engine's, which runs no interleaver
                    }
                } else {
                    measurements = bench::runRepeated(workload, kind, settings.run);
                }
                for (bench::Measurement& measurement : measurements) {
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
#ifdef _WIN32
    // Write '\n' as is, not as CRLF, so the console output has LF line endings like the CSV and JSON files.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    const std::vector<std::string_view> args(argv + 1, argv + argc);
    Settings settings;
    settings.run.engine = bench::engineBesideProgram(argc > 0 ? argv[0] : "");
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
