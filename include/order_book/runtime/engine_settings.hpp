#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "order_book/runtime/durability.hpp"
#include "order_book/runtime/thread_placement.hpp"

// order_book_engine's settings: its whole command line, since it has no config file and reads
// no environment, so a run's flags are its record. See docs/internals/runtime.md.
namespace order_book::runtime {

    struct ListenAddress {
        std::array<std::uint8_t, 4> host;  // IPv4
        std::uint16_t port;                // 0 lets the system pick

        bool operator==(const ListenAddress&) const = default;
    };

    // A spin window of this many nanoseconds means never sleep: --spin-window unlimited.
    inline constexpr std::chrono::nanoseconds unlimitedSpinSetting = std::chrono::nanoseconds::max();

    struct EngineSettings {
        std::string instruments;  // the instrument list's path
        std::uint32_t shards = 0;
        ListenAddress listen{};
        std::size_t commandRing = 4096;
        std::size_t responseRing = 16384;
        std::chrono::nanoseconds spinWindow = std::chrono::microseconds(100);
        std::size_t capacityOrders = 4096;
        std::size_t capacityStops = 1024;
        std::size_t capacityLevels = 256;
        std::size_t maxConnections = 64;  // per gateway thread
        std::uint32_t gatewayThreads = 1;  // never derived from the machine
        std::size_t highWaterMark = 1024 * 1024;
        std::size_t hardLimit = 16 * 1024 * 1024;
        std::chrono::nanoseconds stopGrace = std::chrono::seconds(5);
        // Where each shard's log goes. Empty runs with no log, so a response waits for nothing
        // and a restart loses every book.
        std::string logDir;
        Durability durability = Durability::MACHINE;
        std::size_t logRing = defaultLogRingRecords;  // records per shard
        std::vector<std::uint32_t> pinMatching;    // a core per shard, in shard order; empty is off
        std::vector<std::uint32_t> pinGateway;     // a core per gateway thread; empty is off
        std::vector<std::uint32_t> pinLogWriters;  // a core per shard's log writer; empty is off
        ThreadPriority threadPriority = ThreadPriority::NORMAL;  // matching and gateway threads, and log writers
        bool stopOnStdinClose = false;

        bool operator==(const EngineSettings&) const = default;
    };

    struct UsageError {
        std::string message;

        bool operator==(const UsageError&) const = default;
    };

    // The arguments after the program's name. `--flag value` only, with --stop-on-stdin-close
    // the one flag that takes no value. --instruments, --shards, and --listen are required, a
    // repeated or unknown flag is an error, sizes take a binary K, M, or G, and times must carry
    // a unit (ns, us, ms, or s), all but a bare 0. What each value means is checked at start,
    // where a bad one refuses the start rather than being a usage error.
    std::variant<EngineSettings, UsageError> parseEngineArguments(std::span<const std::string_view> args);

    // The settings as a command line that would give them, every flag in its canonical order with
    // its value, defaults included: the one line a run prints first.
    std::string formatEngineSettings(const EngineSettings& settings);
    std::string formatListenAddress(const ListenAddress& address);
    // A dotted IPv4 address, a colon, and a port, as --listen takes it, and send's --connect.
    std::optional<ListenAddress> parseListenAddress(std::string_view text);

    // What a usage error prints after its message.
    std::string_view engineUsage() noexcept;

}  // namespace order_book::runtime
