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
#include "durable_wait_target.hpp"
#include "engine_process.hpp"
#include "order_book/runtime/matching_loop.hpp"
#include "handoff_target.hpp"
#include "idle_echo_target.hpp"
#include "raw_write_target.hpp"
#include "histogram.hpp"
#include "latency_report.hpp"
#include "noise_floor.hpp"
#include "round_trip_target.hpp"
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
    "usage: order_book_latency_benchmarks [--interval command|hand-off|durable-wait|round-trip|raw-write|idle-echo]\n"
    "                                     [--time service|response] [--rate N]\n"
    "                                     [--workload NAME]... [--size smoke|standard|extended | --commands N]\n"
    "                                     [--runs N] [--warmup N] [--seed N] [--noise-seconds N]\n"
    "                                     [--pin-core N] [--matching-core N] [--gateway-core N]\n"
    "                                     [--receiver-core N] [--writer-core N] [--server-core N]\n"
    "                                     [--priority normal|high] [--spin-window TIME|unlimited]\n"
    "                                     [--durability process|machine] [--log-dir DIR] [--log-files extended|zero-filled]\n"
    "                                     [--sockets poll|completion-port|epoll] [--idle-connections N]\n"
    "                                     [--roll-worker on|off] [--log-file-records N]\n"
    "                                     [--engine PATH] [--discard-seconds N] [--machine-record FILE]\n"
    "                                     [--out-dir DIR] [--allow-unoptimized]\n"
    "       order_book_latency_benchmarks --help\n";

constexpr std::string_view helpText =
    "Times one interval, stamped command by command, and reports its percentiles merged over\n"
    "every run, beside each run's median and range, with a mandatory run record. A row with an\n"
    "unfilled record field, too few runs, or an unoptimized build says so and can't be quoted.\n"
    "Read docs/LATENCY.md before quoting anything.\n"
    "\n"
    "options:\n"
    "  --interval NAME      the interval to time: command, one book call (the default); hand-off,\n"
    "                       into the command ring and back out of the response ring, on a real\n"
    "                       matching thread; durable-wait, from a record entering a shard's log\n"
    "                       ring until the durable position covers it, through a real log writer\n"
    "                       on real files; or round-trip, from the send over loopback through\n"
    "                       the shipped order_book_engine, a fresh child process each run, to the\n"
    "                       frame flagged last; raw-write, one record's bytes written at the next\n"
    "                       offset of one file, opened as the log writer opens its files, with no\n"
    "                       engine; or idle-echo, one 64-byte message out and back over loopback\n"
    "                       through a raw echo thread watching idle connections too, with no gateway\n"
    "  --time KIND          service, closed-loop (the default), or response, on a timetable: 10, 25,\n"
    "                       50, 75, and 90% of the capacity measured that session, then a seeded\n"
    "                       Poisson row at 50%. Open-loop rows also report the driver's lateness.\n"
    "  --rate N             with --time response, one row at N commands a second in place of the\n"
    "                       ladder, with no capacity measured\n"
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
    "  --matching-core N    for hand-off and round-trip, pin the matching thread to this core; needed\n"
    "                       with --pin-core, since a thread that starts on a pinned one may inherit\n"
    "                       its core\n"
    "  --gateway-core N     for round-trip, pin the engine's gateway thread to this core; needed with\n"
    "                       --pin-core\n"
    "  --receiver-core N    for round-trip, pin the thread that reads responses on a timetable to\n"
    "                       this core; needed with --pin-core\n"
    "  --writer-core N      for durable-wait, pin the log writer to this core; needed with --pin-core\n"
    "  --server-core N      for idle-echo, pin the echo thread to this core; needed with --pin-core\n"
    "  --priority LEVEL     normal (the default) or high, for every thread pinned, the engine's\n"
    "                       included; a refusal fails the session\n"
    "  --spin-window TIME   for hand-off, round-trip, and idle-echo, how long the matching thread,\n"
    "                       the engine's threads, or the echo thread looks for work after the last\n"
    "                       before it sleeps, as the engine's --spin-window: 0 always sleeps, and\n"
    "                       unlimited never does (default 100us)\n"
    "  --durability LEVEL   for durable-wait, round-trip, and raw-write: machine (the default), written through\n"
    "                       to the drive, or process, written to the system and flushed when a\n"
    "                       file closes\n"
    "  --log-dir DIR        for durable-wait, round-trip, and raw-write, where each run makes a fresh directory\n"
    "                       for its log, deleted afterwards (default: the system's temporary\n"
    "                       directory)\n"
    "  --log-files FILL     for durable-wait and raw-write, how files are made: extended (the\n"
    "                       default), as the engine makes them, or zero-filled, every byte written\n"
    "                       as zeros and flushed first\n"
    "  --sockets SET        for idle-echo, how the echo thread waits: poll (the default; WSAPoll on\n"
    "                       Windows), completion-port on Windows, or epoll on Linux\n"
    "  --idle-connections N for idle-echo and round-trip, connections open beside the active one\n"
    "                       that never send (default 0; a round trip takes at most 63, filling\n"
    "                       the engine's gateway thread's 64)\n"
    "  --roll-worker WHICH  for durable-wait: on, a roll-forward worker rolling each file the log\n"
    "                       writer closes while the run goes on, or off (the default)\n"
    "  --log-file-records N for durable-wait: records a log file holds (default: the engine's\n"
    "                       256 MiB), so files close, and a worker rolls them, during a run\n"
    "  --engine PATH        for round-trip, the order_book_engine to run (default: the one beside\n"
    "                       this program)\n"
    "  --discard-seconds N  for round-trip, commands sent in each run's first N seconds are sent\n"
    "                       and checked but not recorded (default 2)\n"
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
enum class Interval { COMMAND, HAND_OFF, DURABLE_WAIT, ROUND_TRIP, RAW_WRITE, IDLE_ECHO };

struct Settings {
    TimeKind time = TimeKind::SERVICE;
    Interval interval = Interval::COMMAND;
    std::filesystem::path engine;  // for ROUND_TRIP
    std::uint64_t discardSeconds = 2;
    std::optional<unsigned int> gatewayCore;
    std::optional<unsigned int> receiverCore;
    std::optional<unsigned int> writerCore;  // for DURABLE_WAIT
    std::optional<unsigned int> serverCore;  // for IDLE_ECHO
    std::optional<double> rate;              // one fixed row in place of the ladder
    std::chrono::nanoseconds spinWindow = order_book::runtime::defaultSpinWindow;
    bool spinWindowGiven = false;
    bench::LogFileFill logFiles = bench::LogFileFill::EXTENDED;
    bool logFilesGiven = false;
    latency::EchoSockets sockets = latency::EchoSockets::POLL;
    std::size_t idleConnections = 0;
    bool echoGiven = false;  // --sockets
    bool idleGiven = false;  // --idle-connections
    bool rollWorker = false;
    bool rollWorkerGiven = false;
    std::optional<std::uint64_t> logFileRecords;
    // For DURABLE_WAIT and ROUND_TRIP: the engine's default level, and where each run's log goes.
    bench::RunDurability durability = bench::RunDurability::MACHINE;
    bool durabilityGiven = false;
    std::filesystem::path logParent = bench::defaultLogParent();
    bool logParentGiven = false;
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
                continue;  // so "5ms" isn't read as 5m plus an s
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

// In the largest unit that divides it, as the engine's --spin-window takes it.
std::string formatSpinWindow(std::chrono::nanoseconds window) {
    if (window == order_book::runtime::unlimitedSpin) {
        return "unlimited";
    }
    const std::int64_t nanoseconds = window.count();
    if (nanoseconds == 0) {
        return "0";
    }
    if (nanoseconds % 1'000'000 == 0) {
        return std::to_string(nanoseconds / 1'000'000) + "ms";
    }
    if (nanoseconds % 1'000 == 0) {
        return std::to_string(nanoseconds / 1'000) + "us";
    }
    return std::to_string(nanoseconds) + "ns";
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
                               arg == "--priority" || arg == "--machine-record" || arg == "--out-dir" || arg == "--engine" ||
                               arg == "--durability" || arg == "--log-dir" || arg == "--spin-window" || arg == "--log-files" ||
                               arg == "--sockets" || arg == "--roll-worker";
        const bool numberValue = arg == "--commands" || arg == "--runs" || arg == "--warmup" || arg == "--seed" ||
                                 arg == "--noise-seconds" || arg == "--pin-core" || arg == "--matching-core" ||
                                 arg == "--gateway-core" || arg == "--receiver-core" || arg == "--writer-core" ||
                                 arg == "--discard-seconds" || arg == "--server-core" || arg == "--idle-connections" || arg == "--rate" ||
                                 arg == "--log-file-records";
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
            } else if (arg == "--gateway-core") {
                settings.gatewayCore = static_cast<unsigned int>(*number);
            } else if (arg == "--receiver-core") {
                settings.receiverCore = static_cast<unsigned int>(*number);
            } else if (arg == "--writer-core") {
                settings.writerCore = static_cast<unsigned int>(*number);
            } else if (arg == "--discard-seconds") {
                settings.discardSeconds = *number;
            } else if (arg == "--server-core") {
                settings.serverCore = static_cast<unsigned int>(*number);
            } else if (arg == "--idle-connections") {
                settings.idleConnections = static_cast<std::size_t>(*number);
                settings.idleGiven = true;
            } else if (*number == 0) {
                return std::string(arg) + " must be at least 1";
            } else if (arg == "--runs") {
                settings.runs = static_cast<std::size_t>(*number);
            } else if (arg == "--rate") {
                settings.rate = static_cast<double>(*number);
            } else if (arg == "--log-file-records") {
                if (*number > 100'000'000) {
                    return "--log-file-records takes at most 100000000, past which a file is past the engine's own";
                }
                settings.logFileRecords = *number;
            } else {
                settings.size = "custom";
                settings.commands = static_cast<std::size_t>(*number);
            }
            continue;
        }
        try {
            if (arg == "--interval") {
                if (value != "command" && value != "hand-off" && value != "durable-wait" && value != "round-trip" &&
                    value != "raw-write" && value != "idle-echo") {
                    return "unknown interval '" + std::string(value) +
                           "' (known: command, hand-off, durable-wait, round-trip, raw-write, idle-echo)";
                }
                settings.interval = value == "command"        ? Interval::COMMAND
                                    : value == "hand-off"     ? Interval::HAND_OFF
                                    : value == "durable-wait" ? Interval::DURABLE_WAIT
                                    : value == "round-trip"   ? Interval::ROUND_TRIP
                                    : value == "raw-write"    ? Interval::RAW_WRITE
                                                              : Interval::IDLE_ECHO;
            } else if (arg == "--spin-window") {
                const std::optional<std::chrono::nanoseconds> window = parseSpinWindow(value);
                if (!window) {
                    return "--spin-window needs a time such as 0, 20us, or 1ms, or unlimited, not '" + std::string(value) + "'";
                }
                settings.spinWindow = *window;
                settings.spinWindowGiven = true;
            } else if (arg == "--roll-worker") {
                if (value != "on" && value != "off") {
                    return "unknown --roll-worker '" + std::string(value) + "' (known: on, off)";
                }
                settings.rollWorker = value == "on";
                settings.rollWorkerGiven = true;
            } else if (arg == "--log-files") {
                settings.logFiles = bench::parseLogFileFill(value);
                settings.logFilesGiven = true;
            } else if (arg == "--sockets") {
                const std::optional<latency::EchoSockets> sockets = latency::parseEchoSockets(value);
                if (!sockets) {
#ifdef _WIN32
                    return "unknown sockets '" + std::string(value) + "' (known here: poll, completion-port)";
#else
                    return "unknown sockets '" + std::string(value) + "' (known here: poll, epoll)";
#endif
                }
                settings.sockets = *sockets;
                settings.echoGiven = true;
            } else if (arg == "--durability") {
                if (value != "process" && value != "machine") {
                    return "unknown durability '" + std::string(value) + "' (known: process, machine)";
                }
                settings.durability = bench::parseDurability(value);
                settings.durabilityGiven = true;
            } else if (arg == "--log-dir") {
                settings.logParent = std::filesystem::path(value);
                settings.logParentGiven = true;
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
            } else if (arg == "--engine") {
                settings.engine = std::filesystem::path(value);
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

std::string intervalName(Interval interval) {
    switch (interval) {
        case Interval::COMMAND: return "command";
        case Interval::HAND_OFF: return "hand-off";
        case Interval::DURABLE_WAIT: return "durable-wait";
        case Interval::ROUND_TRIP: return "round-trip";
        case Interval::RAW_WRITE: return "raw-write";
        case Interval::IDLE_ECHO: return "idle-echo";
    }
    return "command";
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
    const Settings& settings = session.settings;
    record.set("interval", intervalName(settings.interval));
    record.set("workload", workload.config.name);
    record.set("workload-fingerprint", std::to_string(bench::workloadFingerprint(workload)));
    record.set("generator-version", std::to_string(bench::workloadGeneratorVersion));
    record.set("seed", std::to_string(settings.seed));
    for (const std::string_view field : {"shards", "gateway-threads", "active-connections", "idle-connections",
                                         "spin-window", "durability", "logs-share-drive"}) {
        record.set(field, "none, one book in process");
    }
    record.set("socket-set", "none, no sockets");
    record.set("matching-cpu-share", "none, no matching thread");
    record.set("log-files", "none, no log");
    const std::string spinWindow = formatSpinWindow(settings.spinWindow);
    if (settings.interval == Interval::HAND_OFF) {
        record.set("shards", "1");
        record.set("gateway-threads", "none, one producer thread in process");
        record.set("active-connections", "none, one producer thread in process");
        record.set("idle-connections", "none, one producer thread in process");
        record.set("spin-window", spinWindow);
        record.set("durability", "none, no log");
        record.set("logs-share-drive", "none, no log");
        // Filled in by each row, from the thread's own CPU time over its runs.
        record.set("matching-cpu-share", "unmeasured");
    } else if (settings.interval == Interval::DURABLE_WAIT) {
        record.set("shards", "1");
        record.set("gateway-threads", "none, one producer thread in process");
        record.set("active-connections", "none, one producer thread in process");
        record.set("idle-connections", "none, one producer thread in process");
        record.set("spin-window", formatSpinWindow(order_book::runtime::defaultSpinWindow) + ", the log writer's");
        record.set("durability", bench::durabilityName(settings.durability));
        record.set("logs-share-drive", settings.rollWorker ? "yes, a roll-forward worker's checkpoints, in the log's directory"
                                                           : "none, no checkpoints: no roll-forward worker runs");
        std::string files = bench::logFileFillName(settings.logFiles);
        if (settings.logFileRecords) {
            files += ", " + std::to_string(*settings.logFileRecords) + " records each";
        }
        if (settings.rollWorker) {
            files += ", a roll-forward worker rolling each as it closes, from an empty book";
        }
        record.set("log-files", files);
    } else if (settings.interval == Interval::ROUND_TRIP) {
        record.set("shards", "1");
        record.set("gateway-threads", "1");
        record.set("active-connections", "1");
        record.set("idle-connections", std::to_string(settings.idleConnections));
        record.set("spin-window", spinWindow);
        record.set("durability", bench::durabilityName(settings.durability));
        record.set("logs-share-drive", "none, no checkpoints until roll-forward lands");
        record.set("socket-set", "poll, the engine's PollSocketSet");
        record.set("matching-cpu-share", "unmeasured, the engine's thread is in another process");
        record.set("log-files", "extended, as the engine makes them");
    } else if (settings.interval == Interval::RAW_WRITE) {
        record.set("shards", "none, no engine");
        for (const std::string_view field : {"gateway-threads", "active-connections", "idle-connections"}) {
            record.set(field, "none, one thread writing one file");
        }
        record.set("spin-window", "none, no thread waits");
        record.set("durability", bench::durabilityName(settings.durability));
        record.set("logs-share-drive", "none, no checkpoints");
        record.set("log-files", bench::logFileFillName(settings.logFiles));
    } else if (settings.interval == Interval::IDLE_ECHO) {
        record.set("shards", "none, no engine");
        record.set("gateway-threads", "none, one raw echo thread");
        record.set("active-connections", "1");
        record.set("idle-connections", std::to_string(settings.idleConnections));
        record.set("spin-window", spinWindow + ", the echo thread's");
        record.set("durability", "none, no log");
        record.set("logs-share-drive", "none, no log");
        record.set("socket-set", latency::echoSocketsName(settings.sockets));
    }
    const std::string senderCore = settings.pinCore ? std::to_string(*settings.pinCore) : "none";
    std::string coreMap = senderCore;
    if (settings.pinCore && settings.interval == Interval::HAND_OFF) {
        coreMap = "sender " + senderCore + ", matching thread " + std::to_string(*settings.matchingCore);
    } else if (settings.pinCore && settings.interval == Interval::DURABLE_WAIT) {
        coreMap = "sender " + senderCore + ", log writer " + std::to_string(*settings.writerCore);
    } else if (settings.pinCore && settings.interval == Interval::IDLE_ECHO) {
        coreMap = "sender " + senderCore + ", echo thread " + std::to_string(*settings.serverCore);
    } else if (settings.pinCore && settings.interval == Interval::ROUND_TRIP) {
        coreMap = "sender " + senderCore + ", receiver " + std::to_string(*settings.receiverCore) + ", engine matching thread " +
                  std::to_string(*settings.matchingCore) + ", engine gateway thread " + std::to_string(*settings.gatewayCore);
    }
    record.set("core-map", coreMap);
    record.set("pinned", session.settings.pinCore ? "yes" : "no");
    record.set("priority", session.settings.highPriority ? "high" : "normal");
    // A round trip's engine was given its declared capacity, which is never 0.
    const order_book::BookCapacity capacity =
        settings.interval == Interval::ROUND_TRIP ? bench::declaredCapacity(workload) : bench::expectedBookSize(workload);
    const bool noBook = settings.interval == Interval::DURABLE_WAIT || settings.interval == Interval::RAW_WRITE ||
                        settings.interval == Interval::IDLE_ECHO;
    record.set("book-capacity", noBook ? std::string("none, no book runs")
                                    : "orders=" + std::to_string(capacity.orders) + " stops=" + std::to_string(capacity.stops) +
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
    return intervalName(session.settings.interval);
}

template <typename Target>
Target makeTarget(const Session& session, const bench::Workload& workload) {
    if constexpr (std::is_same_v<Target, latency::HandOffTarget>) {
        const Settings& settings = session.settings;
        return Target(
            workload,
            [matchingCore = settings.matchingCore, highPriority = settings.highPriority] {
                if (matchingCore) {
                    latency::pinCurrentThread(*matchingCore);
                }
                if (highPriority) {
                    latency::raiseCurrentThreadPriority();
                }
            },
            settings.spinWindow);
    } else if constexpr (std::is_same_v<Target, latency::RoundTripTarget>) {
        const Settings& settings = session.settings;
        latency::RoundTripSetup setup;
        setup.engine = settings.engine;
        if (settings.matchingCore) {
            setup.engineArguments.insert(setup.engineArguments.end(), {"--pin-matching", std::to_string(*settings.matchingCore)});
        }
        if (settings.gatewayCore) {
            setup.engineArguments.insert(setup.engineArguments.end(), {"--pin-gateway", std::to_string(*settings.gatewayCore)});
        }
        if (settings.highPriority) {
            setup.engineArguments.insert(setup.engineArguments.end(), {"--thread-priority", "high"});
        }
        if (settings.spinWindowGiven) {
            setup.engineArguments.insert(setup.engineArguments.end(), {"--spin-window", formatSpinWindow(settings.spinWindow)});
        }
        setup.durability = settings.durability;
        setup.logParent = settings.logParent;
        setup.idleConnections = settings.idleConnections;
        setup.setUpReceiver = [receiverCore = settings.receiverCore, highPriority = settings.highPriority] {
            if (receiverCore) {
                latency::pinCurrentThread(*receiverCore);
            }
            if (highPriority) {
                latency::raiseCurrentThreadPriority();
            }
        };
        return Target(workload, setup);
    } else if constexpr (std::is_same_v<Target, latency::DurableWaitTarget>) {
        const Settings& settings = session.settings;
        return Target(
            workload, settings.durability, settings.logParent,
            [writerCore = settings.writerCore, highPriority = settings.highPriority] {
                if (writerCore) {
                    latency::pinCurrentThread(*writerCore);
                }
                if (highPriority) {
                    latency::raiseCurrentThreadPriority();
                }
            },
            settings.logFiles, settings.rollWorker, settings.logFileRecords);
    } else if constexpr (std::is_same_v<Target, latency::RawWriteTarget>) {
        const Settings& settings = session.settings;
        return Target(workload, settings.durability, settings.logParent, settings.logFiles);
    } else if constexpr (std::is_same_v<Target, latency::IdleEchoTarget>) {
        const Settings& settings = session.settings;
        latency::IdleEchoSetup setup;
        setup.sockets = settings.sockets;
        setup.idleConnections = settings.idleConnections;
        setup.spinWindow = settings.spinWindow;
        setup.setUpServer = [serverCore = settings.serverCore, highPriority = settings.highPriority] {
            if (serverCore) {
                latency::pinCurrentThread(*serverCore);
            }
            if (highPriority) {
                latency::raiseCurrentThreadPriority();
            }
        };
        return Target(workload, setup);
    } else {
        return Target(workload);
    }
}

Ticks discardTicks(const Session& session) {
    return static_cast<Ticks>(static_cast<double>(session.settings.discardSeconds) * 1e9 * session.clockRecord.ticksPerNanosecond);
}

// A round trip's load names its discard, when it has one.
std::string loadWithDiscard(const Session& session, const std::string& load) {
    if (session.settings.interval != Interval::ROUND_TRIP || session.settings.discardSeconds == 0) {
        return load;
    }
    return load + ", commands in each run's first " + std::to_string(session.settings.discardSeconds) + " s not recorded";
}

// A hand-off's matching thread's CPU at a run's start, and when it was read.
struct CpuMark {
    std::uint64_t cpu = 0;
    Ticks at = 0;
};

template <typename Target>
CpuMark markCpu(const Session& session, const Target& target) {
    if constexpr (std::is_same_v<Target, latency::HandOffTarget>) {
        return CpuMark{target.matchingCpu(), session.clock.now()};
    } else {
        static_cast<void>(target);
        return CpuMark{0, session.clock.now()};
    }
}

// The share of a core the matching thread used since mark, or nothing for another target or a
// clock that can't say.
template <typename Target>
std::optional<double> cpuShare(const Session& session, const Target& target, const CpuMark& mark) {
    if constexpr (std::is_same_v<Target, latency::HandOffTarget>) {
        const std::uint64_t used = target.matchingCpu() - mark.cpu;
        const Ticks now = session.clock.now();
        const double span = static_cast<double>(now - mark.at) / session.clockRecord.ticksPerNanosecond;
        return latency::ThreadCpuTime::shareOf(used, span, session.clockRecord.name == latency::tscClockName,
                                               session.clockRecord.ticksPerNanosecond);
    } else {
        static_cast<void>(session);
        static_cast<void>(target);
        static_cast<void>(mark);
        return std::nullopt;
    }
}

// The median run's share of a core, and the range, as percents.
std::string formatShares(std::vector<std::optional<double>> shares) {
    std::vector<double> measured;
    for (const std::optional<double>& share : shares) {
        if (!share) {
#ifdef _WIN32
            return "unmeasured: a thread's cycle time needs the TSC clock";
#else
            return "unmeasured";
#endif
        }
        measured.push_back(*share * 100.0);
    }
    if (measured.empty()) {
        return "unmeasured";
    }
    std::sort(measured.begin(), measured.end());
    return bench::formatFixed(measured[measured.size() / 2], 1) + "% of a core, the median run; " + bench::formatFixed(measured.front(), 1) +
           " to " + bench::formatFixed(measured.back(), 1) + "% over runs";
}

template <typename Target>
latency::ReportRow runClosedLoopRow(const Session& session, const bench::Workload& workload) {
    latency::ReportRow row =
        startRow(session, workload, intervalName(session) + ", service time, " + workload.config.name + ", closed-loop");
    row.record.set("time", "service");
    row.record.set("load", loadWithDiscard(session, "closed-loop"));
    row.record.set("target-rate", "none");
    row.record.set("achieved-rate", "not reported: a per-command timed run never reports throughput");

    Ticks elapsed = 0;
    std::uint64_t growths = 0;
    std::vector<std::optional<double>> shares;
    for (std::size_t run = 0; run < session.settings.warmups + session.settings.runs; ++run) {
        Target target = makeTarget<Target>(session, workload);
        const CpuMark mark = markCpu(session, target);
        const Ticks start = session.clock.now();
        latency::Histogram service;
        if constexpr (std::is_same_v<Target, latency::RoundTripTarget>) {
            // A round trip's first seconds are left out, a placeholder until a warm-up is measured.
            const Ticks discard = discardTicks(session);
            for (std::size_t index = 0; index < workload.timedCount(); ++index) {
                const latency::Stamps stamps = target(index, session.clock);
                if (stamps.started - start >= discard) {
                    service.record(stamps.done - stamps.started);
                }
            }
        } else {
            service = latency::runClosedLoop(workload.timedCount(), session.clock, target);
        }
        const Ticks stop = session.clock.now();
        const std::optional<double> share = cpuShare(session, target, mark);
        target.checkOutcome();
        if (run >= session.settings.warmups) {
            elapsed += stop - start;
            growths += target.capacityGrowths();
            row.runs.push_back(std::move(service));
            shares.push_back(share);
        }
    }
    row.record.set("capacity-growths", formatGrowths(growths));
    if constexpr (std::is_same_v<Target, latency::HandOffTarget>) {
        row.record.set("matching-cpu-share", formatShares(shares));
    }
    row.commandsPerSecond = static_cast<double>(session.settings.runs * workload.timedCount()) /
                            secondsOf(elapsed, session.clockRecord);
    return row;
}

struct Load {
    std::string name;
    double share;  // of capacity
    bool poisson;
};

// One row of response times at a rate, then one of the driver's lateness behind it. A round trip
// adds a row for its responses' first frames between them, since a cascade can put the first well
// before the last.
template <typename Target>
void runTimetableRows(const Session& session, const bench::Workload& workload, const Load& load, double capacity,
                      std::vector<latency::ReportRow>& rows) {
    constexpr bool roundTrip = std::is_same_v<Target, latency::RoundTripTarget>;
    // --rate gives the rate itself, with no capacity measured.
    const std::optional<double>& fixedRate = session.settings.rate;
    const double rate = fixedRate ? *fixedRate : capacity * load.share;
    const double gapTicks = session.clockRecord.ticksPerNanosecond * 1e9 / rate;
    const std::string percent = fixedRate ? bench::formatFixed(rate, 0) + "/s" : std::to_string(static_cast<int>(load.share * 100.0 + 0.5)) + "%";
    const std::string targetRate = fixedRate ? percent + ", as --rate gave"
                                             : bench::formatFixed(rate, 0) + "/s, " + percent + " of capacity measured this session";
    // A round trip's first seconds are left out, a placeholder until a warm-up is measured.
    const std::string loadName = loadWithDiscard(session, load.name);
    const Ticks discard = roundTrip ? discardTicks(session) : 0;
    latency::ReportRow row = startRow(session, workload,
                                      intervalName(session) + ", response time, " + workload.config.name + ", " + load.name + " at " + percent);
    row.record.set("time", "response");
    row.record.set("load", loadName);
    row.record.set("target-rate", targetRate);
    row.openLoop = true;
    latency::ReportRow first = startRow(session, workload,
                                        intervalName(session) + " to the first frame, response time, " + workload.config.name + ", " +
                                            load.name + " at " + percent);
    first.record.set("time", "response, to the response's first frame");
    first.record.set("load", loadName);
    first.record.set("target-rate", targetRate);
    first.openLoop = true;
    latency::ReportRow lateness = startRow(session, workload,
                                           "driver lateness, " + workload.config.name + ", " + load.name + " at " + percent);
    lateness.record.set("time", "driver lateness");
    lateness.record.set("load", loadName);
    lateness.openLoop = true;

    const std::vector<Ticks> schedule =
        load.poisson ? latency::poissonSchedule(workload.timedCount(), gapTicks, session.settings.seed)
                     : latency::fixedRateSchedule(workload.timedCount(), gapTicks);
    Ticks elapsed = 0;
    std::uint64_t growths = 0;
    std::vector<std::optional<double>> shares;
    for (std::size_t run = 0; run < session.settings.warmups + session.settings.runs; ++run) {
        Target target = makeTarget<Target>(session, workload);
        if constexpr (roundTrip) {
            latency::RoundTripRun result = target.runOpenLoop(schedule, session.clock, discard);
            target.checkOutcome();
            if (run >= session.settings.warmups) {
                elapsed += result.elapsed;
                row.sustained = row.sustained && result.sustained;
                row.runs.push_back(std::move(result.response));
                first.runs.push_back(std::move(result.first));
                lateness.runs.push_back(std::move(result.lateness));
            }
        } else {
            const CpuMark mark = markCpu(session, target);
            latency::TimetableRun result = latency::runTimetable(schedule, session.clock, target);
            const std::optional<double> share = cpuShare(session, target, mark);
            target.checkOutcome();
            if (run >= session.settings.warmups) {
                shares.push_back(share);
                elapsed += result.elapsed;
                row.sustained = row.sustained && result.sustained;
                row.runs.push_back(std::move(result.response));
                lateness.runs.push_back(std::move(result.lateness));
            }
        }
        if (run >= session.settings.warmups) {
            growths += target.capacityGrowths();
        }
    }
    row.commandsPerSecond = static_cast<double>(session.settings.runs * workload.timedCount()) /
                            secondsOf(elapsed, session.clockRecord);
    const std::string achieved = bench::formatFixed(row.commandsPerSecond, 0) + "/s";
    row.record.set("achieved-rate", achieved);
    row.record.set("capacity-growths", formatGrowths(growths));
    first.record.set("achieved-rate", achieved);
    first.record.set("capacity-growths", formatGrowths(growths));
    first.commandsPerSecond = row.commandsPerSecond;
    first.sustained = row.sustained;
    lateness.record.set("capacity-growths", formatGrowths(growths));
    lateness.commandsPerSecond = row.commandsPerSecond;
    lateness.sustained = row.sustained;
    lateness.record.set("target-rate", bench::formatFixed(rate, 0) + "/s");
    lateness.record.set("achieved-rate", achieved);
    if constexpr (std::is_same_v<Target, latency::HandOffTarget>) {
        row.record.set("matching-cpu-share", formatShares(shares));
        lateness.record.set("matching-cpu-share", formatShares(shares));
    }
    rows.push_back(std::move(row));
    if constexpr (roundTrip) {
        rows.push_back(std::move(first));
    }
    rows.push_back(std::move(lateness));
}

// Closed-loop, with no per-command stamps, so it is never a throughput claim for a timed run.
// A hand-off or a round trip waits for each answer before sending the next, so its capacity is
// what that target does one command at a time. The queue kind's rate, from batches, is several
// times too high, and so would the gateway kind's be.
template <typename Target>
double measureCapacity(const Session& session, const bench::Workload& workload) {
    std::vector<double> rates;
    for (int run = 0; run < 3; ++run) {
        if constexpr (!std::is_same_v<Target, latency::CommandTarget>) {
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

// A workload's rows: closed-loop service time, or the ladder of response times.
template <typename Target>
void runRows(const Session& session, const bench::Workload& workload, std::vector<latency::ReportRow>& rows) {
    if (session.settings.time == TimeKind::SERVICE) {
        rows.push_back(runClosedLoopRow<Target>(session, workload));
        return;
    }
    if (session.settings.rate) {
        runTimetableRows<Target>(session, workload, Load{"fixed", 0.0, false}, 0.0, rows);
        return;
    }
    const double capacity = measureCapacity<Target>(session, workload);
    for (const Load& load : {Load{"fixed", 0.10, false}, Load{"fixed", 0.25, false}, Load{"fixed", 0.50, false},
                             Load{"fixed", 0.75, false}, Load{"fixed", 0.90, false}, Load{"poisson", 0.50, true}}) {
        runTimetableRows<Target>(session, workload, load, capacity, rows);
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

    if (settings.interval == Interval::HAND_OFF && settings.pinCore.has_value() != settings.matchingCore.has_value()) {
        return usage("--interval hand-off needs both --pin-core and --matching-core, or neither");
    }
    if (settings.interval == Interval::ROUND_TRIP &&
        (settings.pinCore.has_value() != settings.matchingCore.has_value() || settings.pinCore.has_value() != settings.gatewayCore.has_value() ||
         settings.pinCore.has_value() != settings.receiverCore.has_value())) {
        return usage("--interval round-trip needs all of --pin-core, --receiver-core, --matching-core, and --gateway-core, or none");
    }
    if (settings.interval != Interval::ROUND_TRIP && (settings.gatewayCore || settings.receiverCore)) {
        return usage("--gateway-core and --receiver-core are for --interval round-trip");
    }
    if (settings.interval == Interval::DURABLE_WAIT && settings.pinCore.has_value() != settings.writerCore.has_value()) {
        return usage("--interval durable-wait needs both --pin-core and --writer-core, or neither");
    }
    if (settings.interval != Interval::DURABLE_WAIT && settings.writerCore) {
        return usage("--writer-core is for --interval durable-wait");
    }
    if (settings.interval != Interval::DURABLE_WAIT && settings.interval != Interval::ROUND_TRIP &&
        settings.interval != Interval::RAW_WRITE && (settings.durabilityGiven || settings.logParentGiven)) {
        return usage("--durability and --log-dir are for --interval durable-wait, round-trip, and raw-write");
    }
    if (settings.interval != Interval::DURABLE_WAIT && settings.interval != Interval::RAW_WRITE && settings.logFilesGiven) {
        return usage("--log-files is for --interval durable-wait and raw-write");
    }
    if (settings.interval != Interval::HAND_OFF && settings.interval != Interval::ROUND_TRIP &&
        settings.interval != Interval::IDLE_ECHO && settings.spinWindowGiven) {
        return usage("--spin-window is for --interval hand-off, round-trip, and idle-echo");
    }
    if (settings.interval != Interval::IDLE_ECHO && (settings.echoGiven || settings.serverCore)) {
        return usage("--sockets and --server-core are for --interval idle-echo");
    }
    if (settings.interval != Interval::IDLE_ECHO && settings.interval != Interval::ROUND_TRIP && settings.idleGiven) {
        return usage("--idle-connections is for --interval idle-echo and round-trip");
    }
    if (settings.interval != Interval::DURABLE_WAIT && (settings.rollWorkerGiven || settings.logFileRecords)) {
        return usage("--roll-worker and --log-file-records are for --interval durable-wait");
    }
    if (settings.interval == Interval::ROUND_TRIP && settings.idleConnections > 63) {
        return usage("a round trip takes at most 63 idle connections, filling the engine's gateway thread's 64");
    }
    if (settings.interval == Interval::IDLE_ECHO && settings.pinCore.has_value() != settings.serverCore.has_value()) {
        return usage("--interval idle-echo needs both --pin-core and --server-core, or neither");
    }
    if (settings.interval != Interval::HAND_OFF && settings.interval != Interval::ROUND_TRIP && settings.matchingCore) {
        return usage("--matching-core is for --interval hand-off and round-trip");
    }
    if (settings.rate && settings.time != TimeKind::RESPONSE) {
        return usage("--rate needs --time response");
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
        switch (settings.interval) {
            case Interval::COMMAND: runRows<latency::CommandTarget>(session, workload, rows); break;
            case Interval::HAND_OFF: runRows<latency::HandOffTarget>(session, workload, rows); break;
            case Interval::DURABLE_WAIT: runRows<latency::DurableWaitTarget>(session, workload, rows); break;
            case Interval::ROUND_TRIP: runRows<latency::RoundTripTarget>(session, workload, rows); break;
            case Interval::RAW_WRITE: runRows<latency::RawWriteTarget>(session, workload, rows); break;
            case Interval::IDLE_ECHO: runRows<latency::IdleEchoTarget>(session, workload, rows); break;
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
    settings.engine = bench::engineBesideProgram(argc > 0 ? argv[0] : "");
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
