#include "engine_process.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "benchmark_runner.hpp"
#include "child_process.hpp"

namespace order_book::benchmark {

namespace {

// Starting and stopping are quick; these limits only turn a hang into a failure.
constexpr std::chrono::milliseconds startLimit{60'000};
constexpr std::chrono::milliseconds stopLimit{60'000};

std::filesystem::path freshInstrumentListPath() {
    std::random_device entropy;
    const auto now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t tag = (static_cast<std::uint64_t>(entropy()) << 32U) ^ now;
    return std::filesystem::temp_directory_path() / ("order_book_benchmark_instruments_" + std::to_string(tag) + ".csv");
}

std::optional<std::uint64_t> numberAfter(std::string_view line, std::string_view key) {
    const std::size_t at = line.find(key);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view rest = line.substr(at + key.size());
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(rest.data(), rest.data() + rest.size(), value);
    if (parsed.ec != std::errc() || parsed.ptr == rest.data()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::filesystem::path engineBesideProgram(std::string_view programPath) {
#ifdef _WIN32
    constexpr std::string_view engineName = "order_book_engine.exe";
#else
    constexpr std::string_view engineName = "order_book_engine";
#endif
    return std::filesystem::path(programPath).parent_path() / engineName;
}

BookCapacity declaredCapacity(const Workload& workload) {
    const BookCapacity capacity = expectedBookSize(workload);
    return BookCapacity{std::max<std::size_t>(capacity.orders, 1), std::max<std::size_t>(capacity.stops, 1),
                        std::max<std::size_t>(capacity.levels, 1)};
}

std::vector<EngineBookReport> parseStopReport(std::string_view report) {
    std::vector<EngineBookReport> books;
    bool sawStopped = false;
    std::istringstream lines{std::string(report)};
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == "stopped") {
            sawStopped = true;
            continue;
        }
        if (line.rfind("book ", 0) != 0) {
            continue;
        }
        const std::string_view text = line;
        const std::size_t symbolEnd = text.find(' ', 5);
        const std::optional<std::uint64_t> shard = numberAfter(text, " shard=");
        const std::optional<std::uint64_t> orders = numberAfter(text, " orders=");
        const std::optional<std::uint64_t> stops = numberAfter(text, " stops=");
        const std::optional<std::uint64_t> levels = numberAfter(text, " levels=");
        const std::optional<std::uint64_t> growths = numberAfter(text, " growths=");
        if (symbolEnd == std::string_view::npos || !shard || !orders || !stops || !levels || !growths) {
            throw std::runtime_error("engine: a stop report line it can't read: " + line);
        }
        books.push_back(EngineBookReport{std::string(text.substr(5, symbolEnd - 5)), static_cast<std::uint32_t>(*shard),
                                         BookCapacity{static_cast<std::size_t>(*orders), static_cast<std::size_t>(*stops),
                                                      static_cast<std::size_t>(*levels)},
                                         *growths});
    }
    if (!sawStopped) {
        throw std::runtime_error("engine: its stop report has no 'stopped' line");
    }
    return books;
}

EngineProcess::EngineProcess(const std::filesystem::path& engine, const std::vector<Workload>& workloads, const EngineOptions& options)
    : instrumentList(freshInstrumentListPath()) {
    if (options.shards == 0) {
        throw std::invalid_argument("an engine run needs at least one shard");
    }
    if (options.durability == RunDurability::NONE) {
        throw std::invalid_argument("the engine always logs, so its durability is process or machine");
    }
    logDirectory = freshLogDirectory(options.logParent);
    const auto removeFiles = [this] {
        std::error_code ignored;
        std::filesystem::remove(instrumentList, ignored);
        std::filesystem::remove_all(logDirectory, ignored);
    };
    {
        std::ofstream list(instrumentList, std::ios::binary);
        list << "symbol,shard,orders,stops,levels\n";
        for (std::size_t i = 0; i < workloads.size(); ++i) {
            const BookCapacity declared = declaredCapacity(workloads[i]);
            expected.push_back(declared);
            list << instrumentSymbol(i).text() << ',' << i % options.shards << ',' << declared.orders << ',' << declared.stops << ','
                 << declared.levels << '\n';
        }
        list.close();
        if (!list) {
            removeFiles();
            throw std::runtime_error("engine: could not write " + instrumentList.string());
        }
    }

    std::vector<std::string> arguments{"--instruments",         instrumentList.string(), "--shards",     std::to_string(options.shards),
                                       "--listen",              "127.0.0.1:0",           "--log-dir",    logDirectory.string(),
                                       "--durability",          durabilityName(options.durability),
                                       "--stop-on-stdin-close"};
    arguments.insert(arguments.end(), options.extraArguments.begin(), options.extraArguments.end());
    try {
        child = std::make_unique<process_testing::ChildProcess>(engine.string(), arguments);
    } catch (const std::exception& error) {
        removeFiles();
        throw std::runtime_error(std::string("engine: ") + error.what() + " (give its path with --engine)");
    }
    const std::optional<std::string> line = child->readLine(startLimit);
    constexpr std::string_view prefix = "listening 127.0.0.1:";
    const std::optional<std::uint64_t> port =
        line && line->rfind(prefix, 0) == 0 ? numberAfter(*line, prefix) : std::nullopt;
    if (!port || *port == 0 || *port > 65535) {
        const process_testing::ExitStatus status = child->wait(std::chrono::milliseconds(10'000));
        stopped = true;
        removeFiles();
        throw std::runtime_error("engine: " + engine.string() + " did not start (exit " + std::to_string(status.code) +
                                 "; give its path with --engine): " + child->standardError());
    }
    listeningPort = static_cast<std::uint16_t>(*port);
}

EngineProcess::~EngineProcess() {
    child.reset();  // kills the engine if it is still running, so its files are closed
    std::error_code ignored;
    std::filesystem::remove(instrumentList, ignored);
    std::filesystem::remove_all(logDirectory, ignored);
}

void EngineProcess::stop() {
    if (stopped) {
        throw std::logic_error("engine: stopped twice");
    }
    stopped = true;
    child->closeStdin();
    const process_testing::ExitStatus status = child->wait(stopLimit);
    if (status.killed || status.code != 0) {
        throw std::runtime_error("engine: its stop " + std::string(status.killed ? "hung, so it was killed" : "exited " + std::to_string(status.code)) +
                                 ": " + child->standardError());
    }
    const std::vector<EngineBookReport> books = parseStopReport(child->standardOutput());
    if (books.size() != expected.size()) {
        throw std::runtime_error("engine: its stop report names " + std::to_string(books.size()) + " books, not " +
                                 std::to_string(expected.size()));
    }
    // The report sorts books by symbol bytes, which is not B0, B1, ... past B9.
    for (const EngineBookReport& book : books) {
        std::optional<std::size_t> index;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (book.symbol == instrumentSymbol(i).text()) {
                index = i;
            }
        }
        if (!index) {
            throw std::runtime_error("engine: its stop report names a book it was never given: " + book.symbol);
        }
        const BookCapacity& wanted = expected[*index];
        if (book.capacity.orders != wanted.orders || book.capacity.stops != wanted.stops || book.capacity.levels != wanted.levels) {
            throw std::runtime_error("engine: book " + book.symbol + " was warmed to other than its workload's capacity");
        }
        if (book.growths != 0) {
            throw std::runtime_error("engine: book " + book.symbol + " grew past its capacity " + std::to_string(book.growths) +
                                     " times, so the run's capacity was wrong");
        }
    }
}

}  // namespace order_book::benchmark
