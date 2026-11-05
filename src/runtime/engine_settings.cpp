#include "order_book/runtime/engine_settings.hpp"

#include <charconv>
#include <limits>
#include <set>
#include <system_error>

namespace order_book::runtime {

    namespace {
        constexpr std::string_view usageText =
            "usage: order_book_engine --instruments FILE --shards N --listen ADDR:PORT --log-dir DIR\n"
            "                         [--command-ring SLOTS] [--response-ring SLOTS] [--spin-window TIME|unlimited]\n"
            "                         [--capacity-orders N] [--capacity-stops N] [--capacity-levels N]\n"
            "                         [--max-connections N] [--gateway-threads M] [--high-water-mark SIZE] [--hard-limit SIZE]\n"
            "                         [--stop-grace TIME] [--stop-on-stdin-close]\n"
            "                         [--durability machine|process] [--log-file-size SIZE] [--log-ring RECORDS] [--roll-workers N]\n"
            "                         [--checkpoint-dir DIR] [--keep-log]\n"
            "                         [--pin-matching CORES] [--pin-gateway CORES] [--pin-log-writers CORES]\n"
            "                         [--thread-priority normal|high]\n"
            "       order_book_engine --help\n";

        std::optional<std::uint64_t> parseDigits(std::string_view text) {
            std::uint64_t value = 0;
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            if (text.empty() || result.ec != std::errc() || result.ptr != text.data() + text.size()) {
                return std::nullopt;
            }
            return value;
        }

        // Digits, then an optional binary K, M, or G.
        std::optional<std::size_t> parseSize(std::string_view text) {
            std::uint64_t scale = 1;
            if (!text.empty()) {
                switch (text.back()) {
                    case 'K': scale = std::uint64_t{1} << 10; break;
                    case 'M': scale = std::uint64_t{1} << 20; break;
                    case 'G': scale = std::uint64_t{1} << 30; break;
                    default: break;
                }
            }
            if (scale != 1) {
                text.remove_suffix(1);
            }
            const std::optional<std::uint64_t> digits = parseDigits(text);
            if (!digits || *digits > std::numeric_limits<std::size_t>::max() / scale) {
                return std::nullopt;
            }
            return static_cast<std::size_t>(*digits * scale);
        }

        // Digits and a unit, or a bare 0.
        std::optional<std::chrono::nanoseconds> parseTime(std::string_view text) {
            if (text == "0") {
                return std::chrono::nanoseconds::zero();
            }
            constexpr std::pair<std::string_view, std::int64_t> units[] = {{"ns", 1}, {"us", 1'000}, {"ms", 1'000'000}, {"s", 1'000'000'000}};
            for (const auto& [unit, nanoseconds] : units) {
                if (text.size() > unit.size() && text.ends_with(unit)) {
                    const std::string_view digits = text.substr(0, text.size() - unit.size());
                    if (digits.back() < '0' || digits.back() > '9') {
                        continue;  // so "5ms" isn't read as 5m plus an s
                    }
                    const std::optional<std::uint64_t> count = parseDigits(digits);
                    if (!count || *count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / nanoseconds)) {
                        return std::nullopt;
                    }
                    return std::chrono::nanoseconds(static_cast<std::int64_t>(*count) * nanoseconds);
                }
            }
            return std::nullopt;
        }

        // Bytes, in the largest binary unit that divides them.
        std::string formatSize(std::size_t value) {
            constexpr std::pair<char, std::size_t> suffixes[] = {{'G', std::size_t{1} << 30}, {'M', std::size_t{1} << 20}, {'K', std::size_t{1} << 10}};
            for (const auto& [suffix, scale] : suffixes) {
                if (value != 0 && value % scale == 0) {
                    return std::to_string(value / scale) + suffix;
                }
            }
            return std::to_string(value);
        }

        std::string formatTime(std::chrono::nanoseconds time) {
            if (time == unlimitedSpinSetting) {
                return "unlimited";
            }
            const std::int64_t count = time.count();
            if (count == 0) {
                return "0";
            }
            constexpr std::pair<std::string_view, std::int64_t> units[] = {{"s", 1'000'000'000}, {"ms", 1'000'000}, {"us", 1'000}};
            for (const auto& [unit, nanoseconds] : units) {
                if (count % nanoseconds == 0) {
                    return std::to_string(count / nanoseconds) + std::string(unit);
                }
            }
            return std::to_string(count) + "ns";
        }

        // Numbers with commas between, at least one.
        std::optional<std::vector<std::uint32_t>> parseCores(std::string_view text) {
            std::vector<std::uint32_t> cores;
            for (;;) {
                const std::size_t comma = text.find(',');
                const std::optional<std::uint64_t> core = parseDigits(text.substr(0, comma));
                if (!core || *core > std::numeric_limits<std::uint32_t>::max()) {
                    return std::nullopt;
                }
                cores.push_back(static_cast<std::uint32_t>(*core));
                if (comma == std::string_view::npos) {
                    return cores;
                }
                text.remove_prefix(comma + 1);
            }
        }

        std::string formatCores(const std::vector<std::uint32_t>& cores) {
            std::string text;
            for (const std::uint32_t core : cores) {
                text += (text.empty() ? "" : ",") + std::to_string(core);
            }
            return text;
        }

        UsageError needs(std::string_view flag, std::string_view what, std::string_view value) {
            return UsageError{std::string(flag) + " needs " + std::string(what) + ", not '" + std::string(value) + "'"};
        }
    }  // namespace

    std::variant<EngineSettings, UsageError> parseEngineArguments(std::span<const std::string_view> args) {
        EngineSettings settings;
        std::set<std::string_view> seen;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string_view flag = args[i];
            if (!seen.insert(flag).second) {
                return UsageError{"'" + std::string(flag) + "' was given twice"};
            }
            if (flag == "--keep-log") {
                settings.keepLog = true;
                continue;
            }
            if (flag == "--stop-on-stdin-close") {
                settings.stopOnStdinClose = true;
                continue;
            }
            const auto sizeFlag = [&](std::size_t& field) { return &field; };
            std::size_t* size = flag == "--command-ring"      ? sizeFlag(settings.commandRing)
                                : flag == "--response-ring"   ? sizeFlag(settings.responseRing)
                                : flag == "--capacity-orders" ? sizeFlag(settings.capacityOrders)
                                : flag == "--capacity-stops"  ? sizeFlag(settings.capacityStops)
                                : flag == "--capacity-levels" ? sizeFlag(settings.capacityLevels)
                                : flag == "--max-connections" ? sizeFlag(settings.maxConnections)
                                : flag == "--high-water-mark" ? sizeFlag(settings.highWaterMark)
                                : flag == "--hard-limit"      ? sizeFlag(settings.hardLimit)
                                : flag == "--log-file-size"   ? sizeFlag(settings.logFileSize)
                                : flag == "--log-ring"        ? sizeFlag(settings.logRing)
                                                              : nullptr;
            const bool known = size != nullptr || flag == "--instruments" || flag == "--shards" || flag == "--gateway-threads" || flag == "--roll-workers" || flag == "--listen" ||
                               flag == "--spin-window" || flag == "--stop-grace" || flag == "--pin-matching" || flag == "--pin-gateway" ||
                               flag == "--pin-log-writers" || flag == "--thread-priority" || flag == "--log-dir" || flag == "--durability" ||
                               flag == "--checkpoint-dir";
            if (!known) {
                return UsageError{"unknown option '" + std::string(flag) + "'"};
            }
            if (i + 1 == args.size()) {
                return UsageError{std::string(flag) + " needs a value"};
            }
            const std::string_view value = args[++i];
            if (size != nullptr) {
                const std::optional<std::size_t> parsed = parseSize(value);
                if (!parsed) {
                    return needs(flag, "a size: digits, then K, M, or G if you like", value);
                }
                *size = *parsed;
            } else if (flag == "--instruments") {
                settings.instruments = std::string(value);
            } else if (flag == "--log-dir" || flag == "--checkpoint-dir") {
                if (value.empty()) {
                    return needs(flag, "a directory", value);
                }
                (flag == "--log-dir" ? settings.logDir : settings.checkpointDir) = std::string(value);
            } else if (flag == "--durability") {
                if (value != "machine" && value != "process") {
                    return needs(flag, "machine or process", value);
                }
                settings.durability = value == "machine" ? Durability::MACHINE : Durability::PROCESS;
            } else if (flag == "--shards" || flag == "--gateway-threads" || flag == "--roll-workers") {
                const std::optional<std::uint64_t> count = parseDigits(value);
                if (!count || *count > std::numeric_limits<std::uint32_t>::max()) {
                    return needs(flag, "a number", value);
                }
                (flag == "--shards" ? settings.shards : flag == "--gateway-threads" ? settings.gatewayThreads : settings.rollWorkers) =
                    static_cast<std::uint32_t>(*count);
            } else if (flag == "--listen") {
                const std::optional<ListenAddress> address = parseListenAddress(value);
                if (!address) {
                    return needs(flag, "an IPv4 address and port, as 127.0.0.1:0", value);
                }
                settings.listen = *address;
            } else if (flag == "--pin-matching" || flag == "--pin-gateway" || flag == "--pin-log-writers") {
                std::optional<std::vector<std::uint32_t>> cores = parseCores(value);
                if (!cores) {
                    return needs(flag, "cores: numbers with commas between", value);
                }
                (flag == "--pin-matching" ? settings.pinMatching : flag == "--pin-gateway" ? settings.pinGateway : settings.pinLogWriters) =
                    std::move(*cores);
            } else if (flag == "--thread-priority") {
                if (value != "normal" && value != "high") {
                    return needs(flag, "normal or high", value);
                }
                settings.threadPriority = value == "high" ? ThreadPriority::HIGH : ThreadPriority::NORMAL;
            } else {
                const bool spin = flag == "--spin-window";
                const std::optional<std::chrono::nanoseconds> time = spin && value == "unlimited" ? unlimitedSpinSetting : parseTime(value);
                if (!time) {
                    return needs(flag, spin ? "a time with its unit (ns, us, ms, s), or unlimited" : "a time with its unit (ns, us, ms, s)", value);
                }
                (spin ? settings.spinWindow : settings.stopGrace) = *time;
            }
        }
        for (const std::string_view required : {"--instruments", "--shards", "--listen", "--log-dir"}) {
            if (!seen.contains(required)) {
                return UsageError{std::string(required) + " is required"};
            }
        }
        return settings;
    }

    std::optional<ListenAddress> parseListenAddress(std::string_view text) {
        const std::size_t colon = text.rfind(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        const std::optional<std::uint64_t> port = parseDigits(text.substr(colon + 1));
        if (!port || *port > 65535) {
            return std::nullopt;
        }
        ListenAddress address{{}, static_cast<std::uint16_t>(*port)};
        std::string_view host = text.substr(0, colon);
        for (std::size_t part = 0; part < 4; ++part) {
            const std::size_t dot = part == 3 ? host.size() : host.find('.');
            if (dot == std::string_view::npos) {
                return std::nullopt;
            }
            const std::string_view digits = host.substr(0, dot);
            const std::optional<std::uint64_t> value = digits.size() <= 3 ? parseDigits(digits) : std::nullopt;
            if (!value || *value > 255) {
                return std::nullopt;
            }
            address.host[part] = static_cast<std::uint8_t>(*value);
            host = part == 3 ? std::string_view() : host.substr(dot + 1);
        }
        return address;
    }

    std::string formatListenAddress(const ListenAddress& address) {
        return std::to_string(address.host[0]) + '.' + std::to_string(address.host[1]) + '.' + std::to_string(address.host[2]) + '.' +
               std::to_string(address.host[3]) + ':' + std::to_string(address.port);
    }

    std::string formatEngineSettings(const EngineSettings& settings) {
        std::string line = "--instruments " + settings.instruments + " --shards " + std::to_string(settings.shards) + " --listen " +
                           formatListenAddress(settings.listen) + " --command-ring " + std::to_string(settings.commandRing) +
                           " --response-ring " + std::to_string(settings.responseRing) + " --spin-window " + formatTime(settings.spinWindow) +
                           " --capacity-orders " + std::to_string(settings.capacityOrders) + " --capacity-stops " +
                           std::to_string(settings.capacityStops) + " --capacity-levels " + std::to_string(settings.capacityLevels) +
                           " --max-connections " + std::to_string(settings.maxConnections) +
                           " --gateway-threads " + std::to_string(settings.gatewayThreads) +
                           " --high-water-mark " + formatSize(settings.highWaterMark) + " --hard-limit " + formatSize(settings.hardLimit) +
                           " --stop-grace " + formatTime(settings.stopGrace);
        if (!settings.logDir.empty()) {
            line += " --log-dir " + settings.logDir;
        }
        if (!settings.checkpointDir.empty()) {
            line += " --checkpoint-dir " + settings.checkpointDir;
        }
        if (settings.keepLog) {
            line += " --keep-log";
        }
        line += settings.durability == Durability::MACHINE ? " --durability machine" : " --durability process";
        line += " --log-file-size " + formatSize(settings.logFileSize);
        line += " --log-ring " + std::to_string(settings.logRing);
        line += " --roll-workers " + std::to_string(settings.rollWorkers);
        if (!settings.pinMatching.empty()) {
            line += " --pin-matching " + formatCores(settings.pinMatching);
        }
        if (!settings.pinGateway.empty()) {
            line += " --pin-gateway " + formatCores(settings.pinGateway);
        }
        if (!settings.pinLogWriters.empty()) {
            line += " --pin-log-writers " + formatCores(settings.pinLogWriters);
        }
        line += settings.threadPriority == ThreadPriority::HIGH ? " --thread-priority high" : " --thread-priority normal";
        if (settings.stopOnStdinClose) {
            line += " --stop-on-stdin-close";
        }
        return line;
    }

    std::string_view engineUsage() noexcept {
        return usageText;
    }

}  // namespace order_book::runtime
