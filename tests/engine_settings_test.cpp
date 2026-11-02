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

std::vector<std::string_view> required(std::vector<std::string_view> more = {}) {
    std::vector<std::string_view> args{"--instruments", "list.csv", "--shards", "2", "--listen", "127.0.0.1:0"};
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

    for (const std::string_view missing : {"--instruments", "--shards", "--listen"}) {
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
                         "--hard-limit", "2M", "--stop-grace", "250ms", "--stop-on-stdin-close"}));
    expect(settings.commandRing == 8 && settings.responseRing == 2048, "the rings, with a binary K");
    expect(settings.spinWindow == unlimitedSpinSetting, "an unlimited spin window");
    expect(settings.capacityOrders == 1024 * 1024 && settings.capacityStops == 3 && settings.capacityLevels == 1024 * 1024 * 1024,
           "the capacities, with M and G");
    expect(settings.maxConnections == 2 && settings.highWaterMark == 64 * 1024 && settings.hardLimit == 2 * 1024 * 1024, "the connection limits");
    expect(settings.stopGrace == 250ms && settings.stopOnStdinClose, "the stop grace, and the one flag with no value");
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
    expect(parsed({"--instruments", "l", "--shards", "1", "--listen", "0.0.0.0:5000"}).listen == ListenAddress{{0, 0, 0, 0}, 5000},
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
}

// The record each run prints first: every flag with its value, so that it would give the same run.
void testTheRunsRecordGivesTheSameRun() {
    const EngineSettings settings =
        parsed(required({"--high-water-mark", "64K", "--stop-grace", "1500ms", "--spin-window", "unlimited", "--stop-on-stdin-close"}));
    const std::string line = formatEngineSettings(settings);
    expect(line ==
               "--instruments list.csv --shards 2 --listen 127.0.0.1:0 --command-ring 4096 --response-ring 16384 --spin-window unlimited "
               "--capacity-orders 4096 --capacity-stops 1024 --capacity-levels 256 --max-connections 64 --high-water-mark 64K "
               "--hard-limit 16M --stop-grace 1500ms --stop-on-stdin-close",
           "the record names every flag:\n" + line);

    std::vector<std::string> words;
    for (std::size_t start = 0; start < line.size();) {
        const std::size_t space = line.find(' ', start);
        words.push_back(line.substr(start, space - start));
        start = space == std::string::npos ? line.size() : space + 1;
    }
    const std::vector<std::string_view> again(words.begin(), words.end());
    expect(parsed(again) == settings, "and parses back to the same settings");
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
    testTheRunsRecordGivesTheSameRun();
    std::cout << "engine settings tests passed" << std::endl;
    return EXIT_SUCCESS;
}
