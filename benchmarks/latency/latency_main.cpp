#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
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
#include "clock.hpp"
#include "command_target.hpp"
#include "order_book/runtime/matching_loop.hpp"
#include "handoff_target.hpp"
#include "histogram.hpp"
#include "latency_report.hpp"
#include "noise_floor.hpp"
#include "thread_setup.hpp"
#include "timetable.hpp"
#include "workload.hpp"

namespace {

namespace bench = order_book::benchmark;
namespace latency = order_book::latency;
using latency::Ticks;

constexpr int runFailure = 1;  // a run's outcome differed from its workload's, or a file failed
constexpr int usageError = 2;
constexpr int unoptimizedBuild = 3;

constexpr std::string_view usageText =
    "usage: order_book_latency_benchmarks [--interval command|hand-off] [--time service|response]\n"
    "                                     [--workload NAME]... [--size smoke|standard|extended | --commands N]\n"
    "                                     [--runs N] [--warmup N] [--seed N] [--noise-seconds N]\n"
    "                                     [--pin-core N] [--matching-core N] [--priority normal|high]\n"
    "                                     [--machine-record FILE]\n"
    "                                     [--out-dir DIR] [--allow-unoptimized]\n"
    "       order_book_latency_benchmarks --help\n";

constexpr std::string_view helpText =
    "Times one interval, stamped command by command, and reports its percentiles merged over\n"
    "every run, beside each run's median and range, with a mandatory run record. A row with an\n"
    "unfilled record field, too few runs, or an unoptimized build says so and can't be quoted.\n"
    "Read docs/LATENCY.md before quoting anything.\n"
    "\n"
    "options:\n"
    "  --interval NAME      the interval to time: command, one book call (the default), or hand-off,\n"
    "                       into the command ring and back out of the response ring, on a real\n"
    "                       matching thread\n"
    "  --time KIND          service, closed-loop (the default), or response, on a timetable: 10, 25,\n"
    "                       50, 75, and 90% of the capacity measured that session, then a seeded\n"
    "                       Poisson row at 50%. Open-loop rows also report the driver's lateness.\n"
    "  --workload NAME      run this workload (repeatable; default: all eleven)\n"
    "  --size SIZE          commands per run: smoke (10000), standard (100000), or extended\n"
    "                       (1000000, the default)\n"
    "  --commands N         exactly N commands per run, instead of a size\n"
    "  --runs N             measured runs per row (default 9, the fewest a row can be quoted from)\n"
    "  --warmup N           runs before those, discarded (default 1)\n"
    "  --seed N             workload and timetable seed (default 42)\n"
    "  --noise-seconds N    seconds of noise floor before and after the session (default 10; 0 skips\n"
    "                       them, which leaves every row unquotable)\n"
    "  --pin-core N         pin the measuring thread to this core\n"
    "  --matching-core N    for hand-off, pin the matching thread to this core; needed with --pin-core,\n"
    "                       since a thread that starts on a pinned one may inherit its core\n"
    "  --priority LEVEL     normal (the default) or high; a refusal fails the session\n"
    "  --machine-record F   the hand-written machine record, copied into the run record\n"
    "  --out-dir DIR        write report.txt, runs.csv, and every run's histogram counts\n"
    "  --allow-unoptimized  run even in a build with assertions, automatic invariant checks, or a\n"
    "                       sanitizer; every row is then unquotable\n"
    "  -h, --help           print this help and exit\n"
    "\n"
    "exit status:\n"
    "  0  every run completed\n"
    "  1  a run did not do what its workload predicted, a setting was refused, or a file failed\n"
    "  2  usage error\n"
    "  3  the build is not optimized and --allow-unoptimized was not given\n";

enum class TimeKind { SERVICE, RESPONSE };

struct Settings {
    TimeKind time = TimeKind::SERVICE;
    bool handOff = false;  // the hand-off interval, in place of the command interval
    std::vector<std::string> workloads;
    std::string size = bench::benchmarkSizeName(bench::BenchmarkSize::EXTENDED);
    std::size_t commands = bench::timedCommandCount(bench::BenchmarkSize::EXTENDED);
    std::size_t runs = 9;
    std::size_t warmups = 1;
    std::uint64_t seed = 42;
    std::uint64_t noiseSeconds = 10;
    std::optional<unsigned int> pinCore;
    std::optional<unsigned int> matchingCore;
    bool highPriority = false;
    std::optional<std::string> machineRecordPath;
    std::optional<std::string> outDirectory;
    bool allowUnoptimized = false;
    bool help = false;
};

int usage(std::string_view problem) {
    std::cerr << "order_book_latency_benchmarks: " << problem << '\n' << usageText;
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
        if (arg == "--allow-unoptimized") {
            settings.allowUnoptimized = true;
            continue;
        }
        const bool textValue = arg == "--interval" || arg == "--time" || arg == "--workload" || arg == "--size" ||
                               arg == "--priority" || arg == "--machine-record" || arg == "--out-dir";
        const bool numberValue = arg == "--commands" || arg == "--runs" || arg == "--warmup" || arg == "--seed" ||
                                 arg == "--noise-seconds" || arg == "--pin-core" || arg == "--matching-core";
        if (!textValue && !numberValue) {
            return "unknown option '" + std::string(arg) + "'";
        }
        if (i + 1 == args.size()) {
            return std::string(arg) + " needs a value";
        }
        const std::string_view value = args[++i];
        if (numberValue) {
            const std::optional<std::uint64_t> number = parseNumber(value);
            if (!number) {
                return std::string(arg) + " needs a non-negative integer, not '" + std::string(value) + "'";
            }
            if (arg == "--seed") {
                settings.seed = *number;
            } else if (arg == "--warmup") {
                settings.warmups = static_cast<std::size_t>(*number);
            } else if (arg == "--noise-seconds") {
                settings.noiseSeconds = *number;
            } else if (arg == "--pin-core") {
                settings.pinCore = static_cast<unsigned int>(*number);
            } else if (arg == "--matching-core") {
                settings.matchingCore = static_cast<unsigned int>(*number);
            } else if (*number == 0) {
                return std::string(arg) + " must be at least 1";
            } else if (arg == "--runs") {
                settings.runs = static_cast<std::size_t>(*number);
            } else {
                settings.size = "custom";
                settings.commands = static_cast<std::size_t>(*number);
            }
            continue;
        }
        try {
            if (arg == "--interval") {
                if (value != "command" && value != "hand-off") {
                    return "unknown interval '" + std::string(value) + "' (known: command, hand-off)";
                }
                settings.handOff = value == "hand-off";
            } else if (arg == "--time") {
                if (value != "service" && value != "response") {
                    return "unknown time '" + std::string(value) + "' (known: service, response)";
                }
                settings.time = value == "service" ? TimeKind::SERVICE : TimeKind::RESPONSE;
            } else if (arg == "--workload") {
                settings.workloads.emplace_back(bench::findWorkload(value).name);
            } else if (arg == "--size") {
                const bench::BenchmarkSize size = bench::parseBenchmarkSize(value);
                settings.size = bench::benchmarkSizeName(size);
                settings.commands = bench::timedCommandCount(size);
            } else if (arg == "--priority") {
                if (value != "normal" && value != "high") {
                    return "unknown priority '" + std::string(value) + "' (known: normal, high)";
                }
                settings.highPriority = value == "high";
            } else if (arg == "--machine-record") {
                settings.machineRecordPath = std::string(value);
            } else {
                settings.outDirectory = std::string(value);
            }
        } catch (const std::invalid_argument& error) {
            return std::string(error.what());
        }
    }
    if (settings.workloads.empty()) {
        for (const auto* family : {&bench::builtInWorkloads(), &bench::behaviorWorkloads()}) {
            for (const bench::WorkloadConfig& config : *family) {
                settings.workloads.push_back(config.name);
            }
        }
    }
    return std::nullopt;
}

// Writes text in binary mode, so the file has LF line endings on every platform.
void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    file.close();
    if (!file) {
        throw std::runtime_error("could not write " + path.string());
    }
}

std::string readMachineRecord(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("could not read " + path);
    }
    std::string record;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            record += (record.empty() ? "" : "; ") + line;
        }
    }
    return record;
}

// What every row of a session shares.
struct Session {
    Settings settings;
    latency::Clock clock;
    latency::ClockRecord clockRecord;
    bench::BuildInfo build;
    std::string machine;
};

latency::ReportRow startRow(const Session& session, const bench::Workload& workload, const std::string& title) {
    latency::ReportRow row;
    row.title = title;
    row.ticksPerNanosecond = session.clockRecord.ticksPerNanosecond;
    row.stepNanoseconds = session.clockRecord.stepNanoseconds;
    row.emptyPairP50Nanoseconds = session.clockRecord.emptyPairP50Nanoseconds;
    row.clockName = session.clockRecord.name;
    row.optimizedBuild = session.build.optimized();
    row.pinnedAtHighPriority = session.settings.pinCore.has_value() && session.settings.highPriority;

    latency::RunRecord& record = row.record;
    record.set("interval", session.settings.handOff ? "hand-off" : "command");
    record.set("workload", workload.config.name);
    record.set("workload-fingerprint", std::to_string(bench::workloadFingerprint(workload)));
    record.set("generator-version", std::to_string(bench::workloadGeneratorVersion));
    record.set("seed", std::to_string(session.settings.seed));
    for (const std::string_view field : {"shards", "gateway-threads", "active-connections", "idle-connections",
                                         "spin-window", "durability", "logs-share-drive"}) {
        record.set(field, "none, one book in process");
    }
    if (session.settings.handOff) {
        record.set("shards", "1");
        record.set("gateway-threads", "none, one producer thread in process");
        record.set("active-connections", "none, one producer thread in process");
        record.set("idle-connections", "none, one producer thread in process");
        record.set("spin-window", std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(order_book::runtime::defaultSpinWindow).count()) + "us");
        record.set("durability", "none, no log");
        record.set("logs-share-drive", "none, no log");
    }
    const std::string senderCore = session.settings.pinCore ? std::to_string(*session.settings.pinCore) : "none";
    record.set("core-map", session.settings.handOff && session.settings.pinCore
                               ? "sender " + senderCore + ", matching thread " + std::to_string(*session.settings.matchingCore)
                               : senderCore);
    record.set("pinned", session.settings.pinCore ? "yes" : "no");
    record.set("priority", session.settings.highPriority ? "high" : "normal");
    const order_book::BookCapacity capacity = bench::expectedBookSize(workload);
    record.set("book-capacity", "orders=" + std::to_string(capacity.orders) + " stops=" + std::to_string(capacity.stops) +
                                    " levels=" + std::to_string(capacity.levels));
    record.set("clock", session.clockRecord.name);
    record.set("clock-step-ns", bench::formatFixed(session.clockRecord.stepNanoseconds, 1));
    record.set("clock-read-ns", bench::formatFixed(session.clockRecord.readCostNanoseconds, 1));
    record.set("empty-pair-ns", "p50 " + bench::formatFixed(session.clockRecord.emptyPairP50Nanoseconds, 1) + ", p99 " +
                                    bench::formatFixed(session.clockRecord.emptyPairP99Nanoseconds, 1));
    record.set("commands-per-run", std::to_string(session.settings.commands));
    record.set("build", session.build.config + ", " + session.build.compiler);
    if (!session.machine.empty()) {
        record.set("machine", session.machine);
    }
    return row;
}

// Each growth is a tail event, so a nonzero count stands out in the record.
std::string formatGrowths(std::uint64_t growths) {
    return growths == 0 ? "0" : std::to_string(growths) + ", each one a tail event in these runs";
}

double secondsOf(Ticks ticks, const latency::ClockRecord& clock) {
    return static_cast<double>(ticks) / clock.ticksPerNanosecond / 1e9;
}

std::string intervalName(const Session& session) {
    return session.settings.handOff ? "hand-off" : "command";
}

template <typename Target>
Target makeTarget(const Session& session, const bench::Workload& workload) {
    if constexpr (std::is_same_v<Target, latency::HandOffTarget>) {
        const Settings& settings = session.settings;
        return Target(workload, [matchingCore = settings.matchingCore, highPriority = settings.highPriority] {
            if (matchingCore) {
                latency::pinCurrentThread(*matchingCore);
            }
            if (highPriority) {
                latency::raiseCurrentThreadPriority();
            }
        });
    } else {
        return Target(workload);
    }
}

template <typename Target>
latency::ReportRow runClosedLoopRow(const Session& session, const bench::Workload& workload) {
    latency::ReportRow row =
        startRow(session, workload, intervalName(session) + ", service time, " + workload.config.name + ", closed-loop");
    row.record.set("time", "service");
    row.record.set("load", "closed-loop");
    row.record.set("target-rate", "none");
    row.record.set("achieved-rate", "not reported: a per-command timed run never reports throughput");

    Ticks elapsed = 0;
    std::uint64_t growths = 0;
    for (std::size_t run = 0; run < session.settings.warmups + session.settings.runs; ++run) {
        Target target = makeTarget<Target>(session, workload);
        const Ticks start = session.clock.now();
        latency::Histogram service = latency::runClosedLoop(workload.timedCount(), session.clock, target);
        const Ticks stop = session.clock.now();
        target.checkOutcome();
        if (run >= session.settings.warmups) {
            elapsed += stop - start;
            growths += target.capacityGrowths();
            row.runs.push_back(std::move(service));
        }
    }
    row.record.set("capacity-growths", formatGrowths(growths));
    row.commandsPerSecond = static_cast<double>(session.settings.runs * workload.timedCount()) /
                            secondsOf(elapsed, session.clockRecord);
    return row;
}

struct Load {
    std::string name;
    double share;  // of capacity
    bool poisson;
};

// One row of response times at a rate, then one of the driver's lateness behind it.
template <typename Target>
void runTimetableRows(const Session& session, const bench::Workload& workload, const Load& load, double capacity,
                      std::vector<latency::ReportRow>& rows) {
    const double rate = capacity * load.share;
    const double gapTicks = session.clockRecord.ticksPerNanosecond * 1e9 / rate;
    const std::string percent = std::to_string(static_cast<int>(load.share * 100.0 + 0.5)) + "%";
    latency::ReportRow row = startRow(session, workload,
                                      intervalName(session) + ", response time, " + workload.config.name + ", " + load.name + " at " + percent);
    row.record.set("time", "response");
    row.record.set("load", load.name);
    row.record.set("target-rate", bench::formatFixed(rate, 0) + "/s, " + percent + " of capacity measured this session");
    row.openLoop = true;
    latency::ReportRow lateness = startRow(session, workload,
                                           "driver lateness, " + workload.config.name + ", " + load.name + " at " + percent);
    lateness.record.set("time", "driver lateness");
    lateness.record.set("load", load.name);
    lateness.openLoop = true;

    const std::vector<Ticks> schedule =
        load.poisson ? latency::poissonSchedule(workload.timedCount(), gapTicks, session.settings.seed)
                     : latency::fixedRateSchedule(workload.timedCount(), gapTicks);
    Ticks elapsed = 0;
    std::uint64_t growths = 0;
    for (std::size_t run = 0; run < session.settings.warmups + session.settings.runs; ++run) {
        Target target = makeTarget<Target>(session, workload);
        latency::TimetableRun result = latency::runTimetable(schedule, session.clock, target);
        target.checkOutcome();
        if (run >= session.settings.warmups) {
            elapsed += result.elapsed;
            growths += target.capacityGrowths();
            row.sustained = row.sustained && result.sustained;
            row.runs.push_back(std::move(result.response));
            lateness.runs.push_back(std::move(result.lateness));
        }
    }
    row.commandsPerSecond = static_cast<double>(session.settings.runs * workload.timedCount()) /
                            secondsOf(elapsed, session.clockRecord);
    const std::string achieved = bench::formatFixed(row.commandsPerSecond, 0) + "/s";
    row.record.set("achieved-rate", achieved);
    row.record.set("capacity-growths", formatGrowths(growths));
    lateness.record.set("capacity-growths", formatGrowths(growths));
    lateness.commandsPerSecond = row.commandsPerSecond;
    lateness.sustained = row.sustained;
    lateness.record.set("target-rate", bench::formatFixed(rate, 0) + "/s");
    lateness.record.set("achieved-rate", achieved);
    rows.push_back(std::move(row));
    rows.push_back(std::move(lateness));
}

// Closed-loop, with no per-command stamps, so it is never a throughput claim for a timed run.
// A hand-off waits for each answer before sending the next, so its capacity is what that target
// does one command at a time. The queue kind's rate, from batches, is several times too high.
template <typename Target>
double measureCapacity(const Session& session, const bench::Workload& workload) {
    std::vector<double> rates;
    for (int run = 0; run < 3; ++run) {
        if constexpr (std::is_same_v<Target, latency::HandOffTarget>) {
            Target target = makeTarget<Target>(session, workload);
            const Ticks start = session.clock.now();
            latency::runClosedLoop(workload.timedCount(), session.clock, target);
            const Ticks stop = session.clock.now();
            target.checkOutcome();
            rates.push_back(static_cast<double>(workload.timedCount()) / secondsOf(stop - start, session.clockRecord));
        } else {
            rates.push_back(bench::runOnce(workload, bench::BenchmarkKind::CORE).operationsPerSecond());
        }
    }
    std::sort(rates.begin(), rates.end());
    return rates[1];
}

std::string formatRunsCsv(const std::vector<latency::ReportRow>& rows) {
    std::string text = "row,title,run,count,maximum_ticks";
    for (const latency::Percentile percentile : latency::reportedPercentiles) {
        text += ',' + std::string(percentile.name) + "_ticks";
    }
    text += ",sustained,ticks_per_nanosecond\n";
    for (std::size_t r = 0; r < rows.size(); ++r) {
        for (std::size_t run = 0; run < rows[r].runs.size(); ++run) {
            const latency::Histogram& histogram = rows[r].runs[run];
            text += std::to_string(r) + ',' + rows[r].title + ',' + std::to_string(run + 1) + ',' +
                    std::to_string(histogram.count()) + ',' + std::to_string(histogram.maximum());
            for (const latency::Percentile percentile : latency::reportedPercentiles) {
                text += ',' + std::to_string(histogram.valueAt(percentile));
            }
            text += ',' + std::string(rows[r].sustained ? "yes" : "no") + ',' +
                    bench::formatFixed(rows[r].ticksPerNanosecond, 6) + '\n';
        }
    }
    return text;
}

// Raw counts, byte for byte, so every percentile here can be recomputed later.
void writeResults(const std::filesystem::path& directory, const std::string& report,
                  const std::vector<latency::ReportRow>& rows) {
    std::filesystem::create_directories(directory / "counts");
    writeFile(directory / "report.txt", report);
    writeFile(directory / "runs.csv", formatRunsCsv(rows));
    for (std::size_t r = 0; r < rows.size(); ++r) {
        for (std::size_t run = 0; run < rows[r].runs.size(); ++run) {
            writeFile(directory / "counts" /
                          ("row-" + std::to_string(r) + "-run-" + std::to_string(run + 1) + ".csv"),
                      rows[r].runs[run].formatCounts());
        }
    }
}

std::optional<latency::NoiseFloor> noiseFloor(const Session& session) {
    if (session.settings.noiseSeconds == 0) {
        return std::nullopt;
    }
    const double ticks = static_cast<double>(session.settings.noiseSeconds) * 1e9 * session.clockRecord.ticksPerNanosecond;
    return latency::measureNoiseFloor(session.clock, static_cast<Ticks>(ticks), session.clockRecord.ticksPerNanosecond);
}

int runSession(const Settings& settings) {
    const bench::BuildInfo build = bench::currentBuild();
    if (!build.optimized() && !settings.allowUnoptimized) {
        std::cerr << "order_book_latency_benchmarks: this build is not optimized (config " << build.config
                  << ", NDEBUG " << (build.ndebug ? "on" : "off") << ", invariant checks "
                  << (build.invariantChecks ? "on" : "off") << ", AddressSanitizer "
                  << (build.addressSanitizer ? "on" : "off") << ", UBSan "
                  << (build.undefinedBehaviorSanitizer ? "on" : "off") << ").\n"
                  << "Measure a Release build without ORDER_BOOK_INVARIANT_CHECKS=ON or sanitizers, or pass\n"
                  << "--allow-unoptimized to run anyway.\n";
        return unoptimizedBuild;
    }

    if (settings.handOff && settings.pinCore.has_value() != settings.matchingCore.has_value()) {
        return usage("--interval hand-off needs both --pin-core and --matching-core, or neither");
    }
    if (settings.pinCore) {
        latency::pinCurrentThread(*settings.pinCore);
    }
    if (settings.highPriority) {
        latency::raiseCurrentThreadPriority();
    }

    const latency::Clock clock = latency::detectClock();
    Session session{settings, clock, latency::measureClock(clock), build,
                    settings.machineRecordPath ? readMachineRecord(*settings.machineRecordPath) : std::string()};

    const std::optional<latency::NoiseFloor> noiseBefore = noiseFloor(session);
    std::vector<latency::ReportRow> rows;
    for (const std::string& name : settings.workloads) {
        const bench::Workload workload = bench::generateWorkload(bench::findWorkload(name), settings.commands, settings.seed);
        if (settings.time == TimeKind::SERVICE) {
            rows.push_back(session.settings.handOff ? runClosedLoopRow<latency::HandOffTarget>(session, workload)
                                                    : runClosedLoopRow<latency::CommandTarget>(session, workload));
            continue;
        }
        const double capacity = session.settings.handOff ? measureCapacity<latency::HandOffTarget>(session, workload)
                                                         : measureCapacity<latency::CommandTarget>(session, workload);
        for (const Load& load : {Load{"fixed", 0.10, false}, Load{"fixed", 0.25, false}, Load{"fixed", 0.50, false},
                                 Load{"fixed", 0.75, false}, Load{"fixed", 0.90, false}, Load{"poisson", 0.50, true}}) {
            if (session.settings.handOff) {
                runTimetableRows<latency::HandOffTarget>(session, workload, load, capacity, rows);
            } else {
                runTimetableRows<latency::CommandTarget>(session, workload, load, capacity, rows);
            }
        }
    }
    const std::optional<latency::NoiseFloor> noiseAfter = noiseFloor(session);

    std::string report;
    for (latency::ReportRow& row : rows) {
        row.noiseBefore = noiseBefore;
        row.noiseAfter = noiseAfter;
        report += latency::formatRow(row) + '\n';
    }
    std::cout << report;
    std::cout.flush();
    if (settings.outDirectory) {
        writeResults(*settings.outDirectory, report, rows);
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Write '\n' as is, not as CRLF, so the console output has LF line endings like the files.
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
        return runSession(settings);
    } catch (const std::exception& error) {
        std::cout.flush();
        std::cerr << "order_book_latency_benchmarks: " << error.what() << '\n';
        return runFailure;
    }
}
