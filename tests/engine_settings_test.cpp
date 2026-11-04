#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "order_book/runtime/engine_settings.hpp"

// parseEngineArguments and formatEngineSettings, with no process. See docs/internals/runtime.md.
namespace {

using namespace order_book::runtime;
using namespace std::chrono_literals;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

std::variant<EngineSettings, UsageError> parse(std::vector<std::string_view> args) {
    return parseEngineArguments(args);
}

// The required flags, the log directory left out if more gives its own.
std::vector<std::string_view> required(std::vector<std::string_view> more = {}) {
    std::vector<std::string_view> args{"--instruments", "list.csv", "--shards", "2", "--listen", "127.0.0.1:0"};
    if (std::find(more.begin(), more.end(), std::string_view("--log-dir")) == more.end()) {
        args.insert(args.end(), {"--log-dir", "logs"});
    }
    args.insert(args.end(), more.begin(), more.end());
    return args;
}

EngineSettings parsed(std::vector<std::string_view> args) {
    const std::variant<EngineSettings, UsageError> result = parse(std::move(args));
    if (const UsageError* error = std::get_if<UsageError>(&result)) {
        expect(false, "expected settings, got: " + error->message);
    }
    return std::get<EngineSettings>(result);
}

// A run's record split back into its words.
std::vector<std::string> wordsOf(const std::string& line) {
    std::vector<std::string> words;
    for (std::size_t start = 0; start < line.size();) {
        const std::size_t space = line.find(' ', start);
        words.push_back(line.substr(start, space - start));
        start = space == std::string::npos ? line.size() : space + 1;
    }
    return words;
}

std::string usage(std::vector<std::string_view> args) {
    const std::variant<EngineSettings, UsageError> result = parse(std::move(args));
    expect(std::holds_alternative<UsageError>(result), "expected a usage error");
    return std::get<UsageError>(result).message;
}

void testTheRequiredFlagsAndTheDefaults() {
    const EngineSettings settings = parsed(required());
    expect(settings.instruments == "list.csv" && settings.shards == 2, "the list and the shard count");
    expect(settings.listen == ListenAddress{{127, 0, 0, 1}, 0}, "the address, with port 0");
    expect(settings.commandRing == 4096 && settings.responseRing == 16384, "the ring defaults");
    expect(settings.spinWindow == 100us, "the spin window's default");
    expect(settings.capacityOrders == 4096 && settings.capacityStops == 1024 && settings.capacityLevels == 256, "the capacity defaults");
    expect(settings.maxConnections == 64 && settings.highWaterMark == 1024 * 1024 && settings.hardLimit == 16 * 1024 * 1024,
           "the connection defaults");
    expect(settings.stopGrace == 5s && !settings.stopOnStdinClose, "the stop defaults");
    expect(settings.pinMatching.empty() && settings.pinGateway.empty() && settings.threadPriority == ThreadPriority::NORMAL,
           "no pinning, at normal priority");
    expect(settings.gatewayThreads == 1, "one gateway thread, never derived from the machine");
    expect(settings.logDir == "logs", "the log's directory, which is required");
    expect(settings.durability == Durability::MACHINE && settings.logRing == 65536 && settings.pinLogWriters.empty(),
           "written through to the drive, through a ring of 65,536 records, unpinned");
    expect(settings.logFileSize == 256 * 1024 * 1024, "in files of 256 MiB");

    for (const std::string_view missing : {"--instruments", "--shards", "--listen", "--log-dir"}) {
        std::vector<std::string_view> args;
        const std::vector<std::string_view> all = required();
        for (std::size_t i = 0; i < all.size(); i += 2) {
            if (all[i] != missing) {
                args.insert(args.end(), {all[i], all[i + 1]});
            }
        }
        expect(usage(args) == std::string(missing) + " is required", "each required flag is required: " + std::string(missing));
    }
}

void testEveryFlag() {
    const EngineSettings settings =
        parsed(required({"--command-ring", "8", "--response-ring", "2K", "--spin-window", "unlimited", "--capacity-orders", "1M",
                         "--capacity-stops", "3", "--capacity-levels", "1G", "--max-connections", "2", "--high-water-mark", "64K",
                         "--hard-limit", "2M", "--stop-grace", "250ms", "--stop-on-stdin-close", "--gateway-threads", "3"}));
    expect(settings.commandRing == 8 && settings.responseRing == 2048, "the rings, with a binary K");
    expect(settings.spinWindow == unlimitedSpinSetting, "an unlimited spin window");
    expect(settings.capacityOrders == 1024 * 1024 && settings.capacityStops == 3 && settings.capacityLevels == 1024 * 1024 * 1024,
           "the capacities, with M and G");
    expect(settings.maxConnections == 2 && settings.highWaterMark == 64 * 1024 && settings.hardLimit == 2 * 1024 * 1024, "the connection limits");
    expect(settings.stopGrace == 250ms && settings.stopOnStdinClose, "the stop grace, and the one flag with no value");
    expect(settings.gatewayThreads == 3, "the gateway threads");

    const EngineSettings logged =
        parsed(required({"--log-dir", "logs here", "--durability", "process", "--log-ring", "1K", "--log-file-size", "3M"}));
    expect(logged.logDir == "logs here", "the log directory, as given");
    expect(usage(required({"--log-dir", ""})) == "--log-dir needs a directory, not ''", "an empty one is none, which is refused");
    expect(logged.durability == Durability::PROCESS, "process durability");
    expect(logged.logRing == 1024, "the log ring, a size like any ring's");
    expect(logged.logFileSize == 3 * 1024 * 1024, "the log file size, a size");
    expect(usage(required({"--log-file-size", "big"})) == "--log-file-size needs a size: digits, then K, M, or G if you like, not 'big'",
           "and nothing else");
    expect(parsed(required({"--durability", "machine"})).durability == Durability::MACHINE, "machine, said outright");
    expect(usage(required({"--durability", "disk"})) == "--durability needs machine or process, not 'disk'", "two levels");
    expect(usage(required({"--log-ring", "lots"})) == "--log-ring needs a size: digits, then K, M, or G if you like, not 'lots'",
           "a ring takes a size");

    const EngineSettings placed = parsed(required({"--pin-matching", "3,0", "--pin-gateway", "7", "--thread-priority", "high"}));
    expect(placed.pinMatching == std::vector<std::uint32_t>{3, 0} && placed.pinGateway == std::vector<std::uint32_t>{7},
           "the cores, in the order given");
    expect(placed.threadPriority == ThreadPriority::HIGH, "high priority");
    expect(parsed(required({"--thread-priority", "normal"})).threadPriority == ThreadPriority::NORMAL, "normal, said outright");
}

// A count that doesn't match the threads is the start's to refuse; only the shape is checked here.
void testCoresAndPriority() {
    expect(parsed(required({"--pin-matching", "1,1,1"})).pinMatching.size() == 3, "a core may repeat, and the count isn't checked here");
    for (const std::string_view bad : {"", ",", "1,", ",1", "1,,2", "a", "-1", "1 2", "4294967296"}) {
        expect(usage(required({"--pin-matching", bad})) == "--pin-matching needs cores: numbers with commas between, not '" + std::string(bad) + "'",
               "a malformed core list: '" + std::string(bad) + "'");
    }
    expect(usage(required({"--pin-gateway", "x"})) == "--pin-gateway needs cores: numbers with commas between, not 'x'", "the gateway's too");
    expect(usage(required({"--thread-priority", "realtime"})) == "--thread-priority needs normal or high, not 'realtime'", "two priorities");
    expect(parsed(required({"--pin-log-writers", "4,5"})).pinLogWriters == std::vector<std::uint32_t>{4, 5}, "a core per log writer");
    expect(usage(required({"--pin-log-writers", "4,"})) == "--pin-log-writers needs cores: numbers with commas between, not '4,'",
           "the log writers' too");
}

void testTimesNeedAUnit() {
    expect(parsed(required({"--stop-grace", "7s"})).stopGrace == 7s, "seconds");
    expect(parsed(required({"--stop-grace", "7ms"})).stopGrace == 7ms, "milliseconds");
    expect(parsed(required({"--spin-window", "20us"})).spinWindow == 20us, "microseconds");
    expect(parsed(required({"--spin-window", "500ns"})).spinWindow == 500ns, "nanoseconds");
    expect(parsed(required({"--spin-window", "0"})).spinWindow == 0ns, "a bare 0 needs no unit");
    expect(usage(required({"--stop-grace", "5"})) == "--stop-grace needs a time with its unit (ns, us, ms, s), not '5'", "any other number does");
    expect(usage(required({"--spin-window", "5m"})) ==
               "--spin-window needs a time with its unit (ns, us, ms, s), or unlimited, not '5m'",
           "a unit it doesn't know");
    expect(std::holds_alternative<UsageError>(parse(required({"--stop-grace", "unlimited"}))), "only the spin window may be unlimited");
}

void testSizes() {
    expect(usage(required({"--hard-limit", "16MB"})) == "--hard-limit needs a size: digits, then K, M, or G if you like, not '16MB'",
           "a size takes one letter");
    expect(std::holds_alternative<UsageError>(parse(required({"--command-ring", "k"}))), "a suffix needs digits");
    expect(std::holds_alternative<UsageError>(parse(required({"--command-ring", "-1"}))), "no sign");
    expect(std::holds_alternative<UsageError>(parse(required({"--command-ring", "99999999999999999999"}))), "no overflow");
    expect(std::holds_alternative<UsageError>(parse(required({"--hard-limit", "17179869184G"}))), "no overflow through a suffix");
}

void testTheAddress() {
    expect(parsed({"--instruments", "l", "--shards", "1", "--listen", "0.0.0.0:5000", "--log-dir", "logs"}).listen ==
               ListenAddress{{0, 0, 0, 0}, 5000},
           "any IPv4 address and port");
    for (const std::string_view bad : {"localhost:5000", "127.0.0.1", "127.0.0.1:65536", "127.0.0.256:1", "1.2.3:4", "1.2.3.4.5:6", ":5"}) {
        expect(usage({"--instruments", "l", "--shards", "1", "--listen", bad}) ==
                   "--listen needs an IPv4 address and port, as 127.0.0.1:0, not '" + std::string(bad) + "'",
               "a malformed address: " + std::string(bad));
    }
}

void testTheCommandLinesShape() {
    expect(usage(required({"--shards", "3"})) == "'--shards' was given twice", "a repeated flag");
    expect(usage(required({"--stop-on-stdin-close", "--stop-on-stdin-close"})) == "'--stop-on-stdin-close' was given twice",
           "even one with no value");
    expect(usage(required({"--thread-count", "4"})) == "unknown option '--thread-count'", "an unknown flag");
    expect(usage(required({"--stop-grace"})) == "--stop-grace needs a value", "a flag with nothing after it");
    expect(usage(required({"--shards=2"})) == "unknown option '--shards=2'", "--flag value only");
    expect(usage({"--instruments", "l", "--shards", "two", "--listen", "1.2.3.4:5"}) == "--shards needs a number, not 'two'", "a shard count");
    expect(usage(required({"--gateway-threads", "2K"})) == "--gateway-threads needs a number, not '2K'", "a thread count is no size");
    expect(usage(required({"--gateway-threads", "4294967296"})) == "--gateway-threads needs a number, not '4294967296'", "nor past 32 bits");
    expect(parsed(required({"--gateway-threads", "0"})).gatewayThreads == 0, "0 is the start's to refuse, as --shards 0 is");
}

// The record each run prints first: every flag with its value, so that it would give the same run.
void testTheRunsRecordGivesTheSameRun() {
    const EngineSettings settings =
        parsed(required({"--high-water-mark", "64K", "--stop-grace", "1500ms", "--spin-window", "unlimited", "--stop-on-stdin-close"}));
    const std::string line = formatEngineSettings(settings);
    expect(line ==
               "--instruments list.csv --shards 2 --listen 127.0.0.1:0 --command-ring 4096 --response-ring 16384 --spin-window unlimited "
               "--capacity-orders 4096 --capacity-stops 1024 --capacity-levels 256 --max-connections 64 --gateway-threads 1 --high-water-mark 64K "
               "--hard-limit 16M --stop-grace 1500ms --log-dir logs --durability machine --log-file-size 256M --log-ring 65536 "
               "--thread-priority normal "
               "--stop-on-stdin-close",
           "the record names every flag:\n" + line);

    const std::vector<std::string> words = wordsOf(line);
    const std::vector<std::string_view> again(words.begin(), words.end());
    expect(parsed(again) == settings, "and parses back to the same settings");

    const EngineSettings placed =
        parsed(required({"--pin-matching", "2,0", "--pin-gateway", "1,3", "--thread-priority", "high", "--gateway-threads", "2", "--log-dir",
                         "logs", "--durability", "process", "--log-ring", "256", "--pin-log-writers", "4,4", "--log-file-size", "1000"}));
    const std::string placedLine = formatEngineSettings(placed);
    expect(placedLine.find(" --max-connections 64 --gateway-threads 2 ") != std::string::npos, "the gateway threads print:\n" + placedLine);
    expect(placedLine.ends_with("--stop-grace 5s --log-dir logs --durability process --log-file-size 1000 --log-ring 256 --pin-matching 2,0 "
                                "--pin-gateway 1,3 --pin-log-writers 4,4 --thread-priority high"),
           "the pins print only when set, and the directory, level, file size, ring, and priority always:\n" + placedLine);
    expect(line.find("--pin-") == std::string::npos && line.find("--thread-priority normal") != std::string::npos,
           "an unpinned run names its priority and no cores");
    const std::vector<std::string> placedWords = wordsOf(placedLine);
    expect(parsed(std::vector<std::string_view>(placedWords.begin(), placedWords.end())) == placed, "which parses back too");
    expect(formatEngineSettings(parsed(required({"--spin-window", "0", "--stop-grace", "3ns"}))).find("--spin-window 0 ") != std::string::npos,
           "a zero prints bare");
}

}  // namespace

int main() {
    testTheRequiredFlagsAndTheDefaults();
    testEveryFlag();
    testTimesNeedAUnit();
    testSizes();
    testTheAddress();
    testTheCommandLinesShape();
    testCoresAndPriority();
    testTheRunsRecordGivesTheSameRun();
    std::cout << "engine settings tests passed" << std::endl;
    return EXIT_SUCCESS;
}
