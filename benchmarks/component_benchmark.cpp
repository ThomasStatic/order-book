#include "component_benchmark.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <latch>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "benchmark_log.hpp"
#include "benchmark_report.hpp"
#include "benchmark_runner.hpp"
#include "order_book/command.hpp"
#include "order_book/command_log.hpp"
#include "order_book/network/entry_check.hpp"
#include "order_book/network/wire_codec.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#include "order_book/order_index.hpp"
#include "order_book/replay_parser.hpp"
#include "order_book/image_header.hpp"
#include "order_book/runtime/checkpoint.hpp"
#include "order_book/runtime/durability.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/log_files.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/log_writer.hpp"
#include "order_book/runtime/recovery.hpp"
#include "order_book/runtime/shard.hpp"
#include "order_book/runtime/roll_forward.hpp"
#include "order_book/submission_result.hpp"
#include "workload.hpp"

namespace order_book::benchmark {

namespace {

namespace replay = order_book::replay;

using Clock = std::chrono::steady_clock;

// A result nothing else reads must still be made.
volatile std::uint64_t controlSink = 0;

constexpr Price midPrice = 2000000;
constexpr std::size_t cacheLine = 64;

std::int64_t nanosecondsBetween(Clock::time_point start, Clock::time_point stop) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
}

ComponentResult growToResult(std::size_t orders, std::size_t passes) {
    ComponentResult result{"grow-to", "order-index", "full", orders, 0, passes, 0, passes};
    const OrderLocation location(0, Side::BUY, {}, nullptr);
    for (std::size_t pass = 0; pass < passes; ++pass) {
        OrderIndex index;
        index.reserve(orders);
        for (std::size_t id = 1; id <= orders; ++id) {
            index.emplace(static_cast<OrderId>(id), location);
        }
        const std::size_t before = index.capacity();
        const Clock::time_point start = Clock::now();
        index.emplace(static_cast<OrderId>(orders + 1), location);
        const Clock::time_point stop = Clock::now();
        if (index.capacity() == before) {
            throw std::logic_error("grow-to: " + std::to_string(orders) + " orders did not fill the index");
        }
        result.elapsedNanoseconds += nanosecondsBetween(start, stop);
    }
    return result;
}

ComponentResult reserveResult(std::size_t orders, std::size_t passes) {
    ComponentResult result{"reserve-orders", "empty-book", "fresh", orders, 0, passes, 0, passes};
    for (std::size_t pass = 0; pass < passes; ++pass) {
        OrderBook book;
        const Clock::time_point start = Clock::now();
        book.reserve(orders);
        const Clock::time_point stop = Clock::now();
        result.elapsedNanoseconds += nanosecondsBetween(start, stop);
    }
    return result;
}

ComponentResult reserveCapacityResult(std::size_t orders, std::size_t passes) {
    const BookCapacity capacity{orders, orders / 4, orders / 10};
    ComponentResult result{"reserve-capacity", "empty-book", "fresh", orders, capacity.levels, passes, 0, passes};
    for (std::size_t pass = 0; pass < passes; ++pass) {
        OrderBook book;
        const Clock::time_point start = Clock::now();
        book.reserve(capacity);
        const Clock::time_point stop = Clock::now();
        result.elapsedNanoseconds += nanosecondsBetween(start, stop);
    }
    return result;
}

void restOne(OrderBook& book, OrderId id, Price price, Side side) {
    if (book.submitOrder(Order(id, price, 1, 1, side)).getStatus() == SubmissionStatus::REJECTED) {
        throw std::runtime_error("new-level: the book refused a resting order");
    }
}

// A book of `levels` bids and `levels` asks, one order each, two ticks apart, so the bid at the
// touch is midPrice and an odd tick behind it is a gap for a new level.
OrderBook ladderBook(std::size_t levels) {
    OrderBook book;
    book.reserve(levels * 2 + 1000);
    for (std::size_t i = 0; i < levels; ++i) {
        const auto ticks = static_cast<Price>(i * 2);
        restOne(book, static_cast<OrderId>(i * 2 + 1), midPrice - ticks, Side::BUY);
        restOne(book, static_cast<OrderId>(i * 2 + 2), midPrice + 2 + ticks, Side::SELL);
    }
    return book;
}

// A pass rests one order on each of `batch` new levels, timed as one, then cancels them, untimed.
// The batch is small against the book, so the book keeps its shape.
ComponentResult newLevelResult(bool far, std::size_t levels, std::size_t passes) {
    const std::size_t batch = levels / 20 < 100 ? levels / 20 : 100;
    OrderBook book = ladderBook(levels);
    const auto firstId = static_cast<OrderId>(levels * 2 + 1);
    ComponentResult result{far ? "new-level-far" : "new-level-near", "ladder", "two-ticks-apart", levels * 2, levels,
                           passes, 0, passes * batch};
    for (std::size_t pass = 0; pass < passes; ++pass) {
        const Clock::time_point start = Clock::now();
        for (std::size_t i = 0; i < batch; ++i) {
            const auto ticks = static_cast<Price>(i * 2);
            const Price price = far ? midPrice - static_cast<Price>(levels * 2) - ticks : midPrice - 1 - ticks;
            restOne(book, firstId + static_cast<OrderId>(i), price, Side::BUY);
        }
        const Clock::time_point stop = Clock::now();
        result.elapsedNanoseconds += nanosecondsBetween(start, stop);
        for (std::size_t i = 0; i < batch; ++i) {
            if (!book.cancelOrder(firstId + static_cast<OrderId>(i)).isSuccess()) {
                throw std::runtime_error("new-level: the book lost an order it rested");
            }
        }
    }
    return result;
}

replay::Symbol componentSymbol() {
    return *replay::Symbol::parse("BENCH");
}

ComponentResult logEncodeResult(const Workload& workload, const std::vector<replay::ReplayRecord>& records, std::size_t passes) {
    ComponentResult result{"log-encode", workload.config.name, "chained", 0, 0, passes, 0, passes * records.size()};
    std::vector<std::array<std::byte, replay::logRecordBytes>> sink(1);
    std::uint32_t checksum = 0;
    for (std::size_t pass = 0; pass < passes; ++pass) {
        const Clock::time_point start = Clock::now();
        for (const replay::ReplayRecord& record : records) {
            checksum = replay::encodeLogRecord(record.symbol, record.command, checksum, sink[0]);
        }
        result.elapsedNanoseconds += nanosecondsBetween(start, Clock::now());
    }
    controlSink = checksum;
    return result;
}

template <typename Reader>
ComponentResult readResult(const char* operation, const Workload& workload, const std::string& bytes, std::size_t records,
                           std::size_t passes) {
    ComponentResult result{operation, workload.config.name, "in-memory", 0, 0, passes, 0, passes * records};
    for (std::size_t pass = 0; pass < passes; ++pass) {
        std::istringstream input(bytes);
        Reader reader(input);
        std::size_t read = 0;
        const Clock::time_point start = Clock::now();
        while (std::holds_alternative<replay::ReplayRecord>(reader.next())) {
            ++read;
        }
        result.elapsedNanoseconds += nanosecondsBetween(start, Clock::now());
        if (read != records) {
            throw std::runtime_error(std::string(operation) + ": read " + std::to_string(read) + " records, not " + std::to_string(records));
        }
    }
    return result;
}

std::string renderCsv(const std::vector<replay::ReplayRecord>& records) {
    std::string text(replay::expectedHeaderV3);
    text += '\n';
    for (const replay::ReplayRecord& record : records) {
        text += replay::formatReplayRow(record);
        text += '\n';
    }
    return text;
}

ComponentResult executeCommandResult(const Workload& workload, const std::vector<replay::ReplayRecord>& records, std::size_t passes) {
    ComponentResult result{"execute-command", workload.config.name, "warm", 0, 0, passes, 0, passes * records.size()};
    for (std::size_t pass = 0; pass < passes; ++pass) {
        OrderBook book;
        preloadBook(book, workload);
        replay::CommandResult commandResult;
        commandResult.reserve(512, 1024);
        const Clock::time_point start = Clock::now();
        for (const replay::ReplayRecord& record : records) {
            replay::executeCommand(book, record.command, commandResult);
        }
        result.elapsedNanoseconds += nanosecondsBetween(start, Clock::now());
    }
    return result;
}

ComponentResult symbolCompareResult(std::size_t passes) {
    const std::vector<replay::Symbol> symbols{*replay::Symbol::parse("AAPL"), *replay::Symbol::parse("MSFT"),
                                              *replay::Symbol::parse("BRK.B"), *replay::Symbol::parse("ES-Z26")};
    constexpr std::size_t comparisons = 100000000;
    ComponentResult result{"symbol-compare", "four-symbols", "word", 0, 0, passes, 0, passes * comparisons};
    std::uint64_t matches = 0;
    for (std::size_t pass = 0; pass < passes; ++pass) {
        const Clock::time_point start = Clock::now();
        for (std::size_t i = 0; i < comparisons; ++i) {
            matches += symbols[i & 3] == symbols[(i >> 2) & 3] ? 1U : 0U;
        }
        result.elapsedNanoseconds += nanosecondsBetween(start, Clock::now());
    }
    controlSink = matches;
    return result;
}

// A Hello, then each record as a Command frame.
std::vector<std::byte> commandFrames(const std::vector<replay::ReplayRecord>& records) {
    std::vector<std::byte> bytes(network::helloFrameBytes + records.size() * network::commandFrameBytes);
    std::span<std::byte> out(bytes);
    out = out.subspan(network::encodeHello(1, network::wireVersion, out));
    for (const replay::ReplayRecord& record : records) {
        out = out.subspan(network::encodeCommand(record.lineNumber, record.symbol, record.command, out));
    }
    return bytes;
}

ComponentResult frameDecodeResult(const Workload& workload, const std::vector<replay::ReplayRecord>& records, std::size_t passes) {
    const std::vector<std::byte> bytes = commandFrames(records);
    ComponentResult result{"frame-decode", workload.config.name, "in-memory", 0, 0, passes, 0, passes * records.size()};
    for (std::size_t pass = 0; pass < passes; ++pass) {
        network::FrameReader reader(network::FrameSource::CLIENT);
        reader.feed(bytes);
        std::size_t read = 0;
        const Clock::time_point start = Clock::now();
        while (std::holds_alternative<network::Frame>(reader.next())) {
            ++read;
        }
        result.elapsedNanoseconds += nanosecondsBetween(start, Clock::now());
        if (read != records.size() + 1) {
            throw std::runtime_error("frame-decode read " + std::to_string(read) + " frames");
        }
    }
    return result;
}

// Every event the workload's commands make on a book that starts empty.
std::vector<BookEvent> eventsOf(const std::vector<replay::ReplayRecord>& records) {
    OrderBook book;
    replay::CommandResult result;
    std::vector<BookEvent> events;
    for (const replay::ReplayRecord& record : records) {
        replay::executeCommand(book, record.command, result);
        events.insert(events.end(), result.events().begin(), result.events().end());
    }
    return events;
}

ComponentResult eventEncodeResult(const Workload& workload, const std::vector<replay::ReplayRecord>& records, std::size_t passes) {
    const std::vector<BookEvent> events = eventsOf(records);
    const replay::Symbol symbol = componentSymbol();
    std::array<std::byte, network::maxEventFrameBytes> out{};
    ComponentResult result{"event-encode", workload.config.name, "in-memory", 0, 0, passes, 0, passes * events.size()};
    std::size_t written = 0;
    for (std::size_t pass = 0; pass < passes; ++pass) {
        const Clock::time_point start = Clock::now();
        for (const BookEvent& event : events) {
            written += network::encodeEvent(1, symbol, event, true, out);
        }
        result.elapsedNanoseconds += nanosecondsBetween(start, Clock::now());
    }
    controlSink = written;
    return result;
}

ComponentResult checkEntryResult(const Workload& workload, const std::vector<replay::ReplayRecord>& records, std::size_t passes) {
    const runtime::InstrumentDirectory directory(
        {runtime::Instrument{componentSymbol(), std::nullopt, std::nullopt, std::nullopt, std::nullopt}}, 1);
    const std::vector<std::byte> bytes = commandFrames(records);
    std::vector<network::CommandFrame> frames;
    frames.reserve(records.size());
    network::FrameReader reader(network::FrameSource::CLIENT);
    reader.feed(bytes);
    while (true) {
        const network::FrameResult read = reader.next();
        const network::Frame* frame = std::get_if<network::Frame>(&read);
        if (frame == nullptr) {
            break;
        }
        if (frame->type == network::FrameType::COMMAND) {
            frames.push_back(network::decodeCommandFrame(*frame));
        }
    }
    ComponentResult result{"check-entry", workload.config.name, "in-memory", 0, 0, passes, 0, passes * frames.size()};
    std::uint64_t accepted = 0;
    for (std::size_t pass = 0; pass < passes; ++pass) {
        const Clock::time_point start = Clock::now();
        for (const network::CommandFrame& frame : frames) {
            accepted += std::holds_alternative<network::EntryAccepted>(network::checkEntry(frame, directory, false)) ? 1U : 0U;
        }
        result.elapsedNanoseconds += nanosecondsBetween(start, Clock::now());
    }
    controlSink = accepted;
    return result;
}

// Hits only, cycling through the list, on one thread.
ComponentResult symbolLookupResult(std::size_t instruments, std::size_t passes) {
    std::vector<runtime::Instrument> list;
    std::vector<replay::Symbol> symbols;
    for (std::size_t i = 0; i < instruments; ++i) {
        symbols.push_back(*replay::Symbol::parse("S" + std::to_string(i)));
        list.push_back(runtime::Instrument{symbols.back(), std::nullopt, std::nullopt, std::nullopt, std::nullopt});
    }
    const runtime::InstrumentDirectory directory(list, 4);
    constexpr std::size_t lookups = std::size_t{1} << 20;
    ComponentResult result{"symbol-lookup", std::to_string(instruments) + "-instruments", "hit", 0, 0,
                           passes, 0, passes * lookups};
    std::uint64_t found = 0;
    for (std::size_t pass = 0; pass < passes; ++pass) {
        const Clock::time_point start = Clock::now();
        for (std::size_t i = 0; i < lookups; ++i) {
            found += directory.find(symbols[i % instruments])->slot;
        }
        result.elapsedNanoseconds += nanosecondsBetween(start, Clock::now());
    }
    controlSink = found;
    return result;
}

// xorshift64: no load or store, so only the core's clock can change how long this takes.
std::uint64_t cpuLoop(std::size_t iterations) {
    std::uint64_t state = 88172645463325252ULL;
    for (std::size_t i = 0; i < iterations; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
    }
    return state;
}

// One slot per cache line, each holding the index of the next, in one random cycle over all of
// them (Sattolo's shuffle), from raw draws so it is the same on every platform.
std::vector<std::size_t> buildWalk(std::size_t bytes, std::uint64_t seed) {
    const std::size_t stride = cacheLine / sizeof(std::size_t);
    const std::size_t slots = bytes / cacheLine;
    std::vector<std::size_t> order(slots);
    for (std::size_t i = 0; i < slots; ++i) {
        order[i] = i;
    }
    std::mt19937_64 random(seed);
    for (std::size_t i = slots - 1; i > 0; --i) {
        std::swap(order[i], order[static_cast<std::size_t>(random() % i)]);
    }
    std::vector<std::size_t> walk(slots * stride);
    for (std::size_t i = 0; i < slots; ++i) {
        walk[order[i] * stride] = order[(i + 1) % slots] * stride;
    }
    return walk;
}

std::size_t memoryWalk(const std::vector<std::size_t>& walk, std::size_t steps) {
    std::size_t at = 0;
    for (std::size_t i = 0; i < steps; ++i) {
        at = walk[at];
    }
    return at;
}

std::string sizeLabel(std::size_t bytes) {
    return std::to_string(bytes / 1024) + "KiB";
}

std::string threadLabel(std::size_t thread, std::size_t threads) {
    return "thread-" + std::to_string(thread) + "-of-" + std::to_string(threads);
}

// Runs body(thread) on every thread at once, after each has been set up, and returns each one's
// time. The first exception a thread raised is rethrown here.
template <typename Body>
std::vector<std::int64_t> runTogether(const ControlSettings& settings, Body body) {
    std::vector<std::int64_t> elapsed(settings.threads, 0);
    std::vector<std::exception_ptr> failures(settings.threads);
    std::latch ready(static_cast<std::ptrdiff_t>(settings.threads));
    std::vector<std::thread> threads;
    for (std::size_t thread = 0; thread < settings.threads; ++thread) {
        threads.emplace_back([&, thread] {
            try {
                if (settings.setUpThread) {
                    settings.setUpThread(thread);
                }
            } catch (...) {
                failures[thread] = std::current_exception();
            }
            ready.arrive_and_wait();
            if (failures[thread]) {
                return;
            }
            const Clock::time_point start = Clock::now();
            body(thread);
            elapsed[thread] = nanosecondsBetween(start, Clock::now());
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (const std::exception_ptr& failure : failures) {
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    return elapsed;
}

// ---- The images group --------------------------------------------------------------------

// Writes and reads every cache line of a buffer larger than the caches, untimed, so the next
// pass starts from memory.
void evictCaches(std::size_t bytes) {
    static std::vector<std::uint64_t> buffer;
    buffer.resize(bytes / sizeof(std::uint64_t));
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < buffer.size(); i += cacheLine / sizeof(std::uint64_t)) {
        buffer[i] += i;
        sum += buffer[i];
    }
    controlSink = controlSink + sum;
}

// A v1 workload's command on a book, which must do as the workload predicts.
void applyCommand(OrderBook& book, const BenchmarkCommand& command) {
    if (command.type == CommandType::SUBMIT) {
        if (book.submitOrder(Order(command.orderId, command.price, command.quantity, 1, command.side)).getStatus() ==
            SubmissionStatus::REJECTED) {
            throw std::runtime_error("images: the book refused a workload's order");
        }
    } else if (!book.cancelOrder(command.orderId).isSuccess()) {
        throw std::runtime_error("images: the book could not cancel a workload's order");
    }
}

std::vector<std::byte> imageOf(const OrderBook& book) {
    std::vector<std::byte> image(book.imageSize());
    if (book.writeImage(image) != image.size()) {
        throw std::runtime_error("images: writeImage did not fill imageSize bytes");
    }
    return image;
}

OrderBook restoredFrom(std::span<const std::byte> image) {
    auto restored = OrderBook::restore(image);
    if (const auto* error = std::get_if<RestoreError>(&restored)) {
        throw std::runtime_error(std::string("images: restore refused a book's own image: ") + error->message);
    }
    return std::move(std::get<OrderBook>(restored));
}

// One operation on one state's book, one per pass, the caches evicted before each.
template <typename Operation>
ComponentResult imageResult(const char* operation, const std::string& workload, const char* state, const OrderBook& book,
                            const ImageSettings& settings, Operation run) {
    ComponentResult result{operation, workload, state, book.activeOrderCount(), book.holding().levels, settings.passes, 0,
                           settings.passes};
    for (std::size_t pass = 0; pass < settings.passes; ++pass) {
        evictCaches(settings.evictionBytes);
        result.elapsedNanoseconds += run();
    }
    return result;
}

void addImageResults(std::vector<ComponentResult>& results, const std::string& workload, const char* state,
                     const OrderBook& book, const ImageSettings& settings) {
    const std::vector<std::byte> image = imageOf(book);
    std::vector<std::byte> out(image.size());
    results.push_back(imageResult("write-image", workload, state, book, settings, [&] {
        const Clock::time_point start = Clock::now();
        const std::size_t written = book.writeImage(out);
        const Clock::time_point stop = Clock::now();
        if (written != out.size()) {
            throw std::runtime_error("images: writeImage did not fill its buffer");
        }
        return nanosecondsBetween(start, stop);
    }));
    results.push_back(imageResult("restore", workload, state, book, settings, [&] {
        const Clock::time_point start = Clock::now();
        auto restored = OrderBook::restore(image);
        const Clock::time_point stop = Clock::now();
        if (!std::holds_alternative<OrderBook>(restored)) {
            throw std::runtime_error("images: restore refused a book's own image");
        }
        return nanosecondsBetween(start, stop);  // the restored book is destroyed untimed
    }));
    results.push_back(imageResult("check-invariants", workload, state, book, settings, [&] {
        const Clock::time_point start = Clock::now();
        const InvariantResult checked = book.checkInvariants();
        const Clock::time_point stop = Clock::now();
        if (!checked.isValid()) {
            throw std::runtime_error("images: a book failed its own check");
        }
        return nanosecondsBetween(start, stop);
    }));
    results.push_back(imageResult("image-proof", workload, state, book, settings, [&] {
        const Clock::time_point start = Clock::now();
        std::optional<OrderBook> proved;
        bool same = false;
        {
            auto restored = OrderBook::restore(image);
            if (auto* rebuilt = std::get_if<OrderBook>(&restored)) {
                same = rebuilt->writeImage(out) == image.size() && out == image;
                proved.emplace(std::move(*rebuilt));
            }
        }
        const Clock::time_point stop = Clock::now();
        if (!same) {
            throw std::runtime_error("images: a book's image did not prove");
        }
        return nanosecondsBetween(start, stop);
    }));
}

void addShapeResults(std::vector<ComponentResult>& results, const char* name, std::size_t orders,
                     const ImageSettings& settings) {
    WorkloadConfig config = findWorkload(name);
    config.initialOrders = orders;
    config.targetLevels = static_cast<unsigned int>(std::min<std::size_t>(config.targetLevels, orders / 2));
    const Workload workload = generateWorkload(config, settings.agingCommands, settings.seed);

    OrderBook preloaded;
    preloadBook(preloaded, workload);
    addImageResults(results, name, "restored", restoredFrom(imageOf(preloaded)), settings);

    OrderBook shuffled;
    shuffled.reserve(BookCapacity{workload.preload.size(), 0, config.targetLevels});
    std::vector<BenchmarkCommand> preload = workload.preload;
    std::shuffle(preload.begin(), preload.end(), std::mt19937_64(settings.seed));
    for (const BenchmarkCommand& command : preload) {
        applyCommand(shuffled, command);
    }
    addImageResults(results, name, "shuffled", shuffled, settings);

    for (const BenchmarkCommand& command : workload.timed) {
        applyCommand(preloaded, command);
    }
    addImageResults(results, name, "aged", preloaded, settings);
}

// A directory of the durability group's own, removed with everything in it once done.
class ScratchDirectory {
public:
    explicit ScratchDirectory(const std::filesystem::path& parent)
        : where(freshLogDirectory(parent.empty() ? defaultLogParent() : parent)) {}
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    ~ScratchDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(where, ignored);
    }
    const std::filesystem::path& path() const noexcept { return where; }

private:
    std::filesystem::path where;
};

void throwOnReport(const std::string& line) {
    throw std::runtime_error("durability: " + line);
}

ComponentResult publishResult(std::size_t orders, const DurabilitySettings& settings) {
    WorkloadConfig config = findWorkload("deep-book");
    config.initialOrders = orders;
    config.targetLevels = static_cast<unsigned int>(std::min<std::size_t>(config.targetLevels, orders / 2));
    OrderBook book;
    preloadBook(book, generateWorkload(config, 1, settings.seed));
    const std::vector<std::byte> image = imageOf(book);

    const ScratchDirectory directory(settings.logParent);
    runtime::FileLogStorage storage(directory.path().string());
    runtime::CheckpointHeader header;
    header.shardCount = 1;
    header.symbol = componentSymbol();
    ComponentResult result{"publish-checkpoint", "deep-book", "log-drive", book.activeOrderCount(), book.holding().levels,
                           settings.publishPasses, 0, settings.publishPasses};
    for (std::size_t pass = 0; pass < settings.publishPasses; ++pass) {
        header.position = pass + 1;
        const Clock::time_point start = Clock::now();
        runtime::publishCheckpoint(storage, header, image);
        result.elapsedNanoseconds += nanosecondsBetween(start, Clock::now());
        storage.remove(runtime::checkpointFileName(header.symbol, header.position));
    }
    return result;
}

std::vector<ComponentResult> rollResults(const DurabilitySettings& settings) {
    if (settings.instruments == 0) {
        throw std::invalid_argument("durability: a roll pass needs instruments");
    }
    std::vector<runtime::Instrument> list;
    for (std::size_t i = 0; i < settings.instruments; ++i) {
        list.push_back(runtime::Instrument{instrumentSymbol(i), 0, std::nullopt, std::nullopt, std::nullopt});
    }
    const runtime::InstrumentDirectory instruments(list, 1);
    const runtime::LogFileShape shape = runtime::logFileShape(instruments, 0, runtime::Durability::PROCESS, runtime::defaultLogFileBytes);
    const std::size_t records = settings.fileRecords != 0 ? settings.fileRecords : static_cast<std::size_t>(shape.records);
    const std::uint64_t fileBytes = shape.headerBytes + records * replay::logRecordBytes;

    const ScratchDirectory directory(settings.logParent);
    runtime::FileLogStorage storage(directory.path().string());
    for (std::size_t i = 0; i < settings.instruments; ++i) {
        runtime::publishEmptyCheckpoint(storage, instrumentSymbol(i), 1, 0, 0);
    }
    {
        // Two full files and the next made ahead, so both are closed, written book by book, each
        // with a stream of its own, so only one book's commands are in memory at a time. The
        // first is rolled; recovery restores at its end and replays the second.
        runtime::Housekeeping housekeeping(storage, instruments, runtime::Durability::PROCESS, fileBytes);
        runtime::Ring<runtime::LogEntry> ring(65536);
        runtime::DurablePosition durable(0);
        runtime::LogWriter writer(housekeeping, 0, ring, durable, 0);
        const auto drain = [&] {
            while (writer.pass() != 0 || housekeeping.pass() != 0) {
            }
        };
        const WorkloadConfig config = findWorkload(settings.workload);
        for (std::size_t i = 0; i < settings.instruments; ++i) {
            const std::size_t share =
                2 * records / settings.instruments + (i + 1 == settings.instruments ? 2 * records % settings.instruments : 0);
            const std::vector<replay::ReplayRecord> commands =
                workloadRecords(generateWorkload(config, share, settings.seed + i), instrumentSymbol(i));
            const auto slot = instruments.find(instrumentSymbol(i))->slot;
            for (std::size_t c = 0; c < share; ++c) {
                const runtime::LogEntry entry{commands[c].command, slot, runtime::ControlKind::COMMAND};
                while (!ring.tryPush(entry)) {
                    drain();
                }
            }
        }
        drain();
        if (durable.load() != 2 * records) {
            throw std::runtime_error("durability: the log files hold " + std::to_string(durable.load()) + " commands, not " +
                                     std::to_string(2 * records));
        }
    }

    ComponentResult result{"roll-pass", settings.workload, "closed-file", 0, 0, settings.rollPasses, 0, settings.rollPasses * records};
    for (std::size_t pass = 0; pass < settings.rollPasses; ++pass) {
        runtime::ShardRoller roller(storage, instruments, 0, throwOnReport);  // reads the empty checkpoints, untimed
        const Clock::time_point start = Clock::now();
        const std::size_t made = roller.roll(records);
        result.elapsedNanoseconds += nanosecondsBetween(start, Clock::now());
        if (made != 1 || roller.counts().published != settings.instruments) {
            throw std::runtime_error("durability: a roll pass did not publish every book once");
        }
        // Every book back at its empty checkpoint for the next pass, counting what they held.
        result.orders = 0;
        result.levels = 0;
        for (std::size_t i = 0; i < settings.instruments; ++i) {
            const std::string name = runtime::checkpointFileName(instrumentSymbol(i), records);
            const std::unique_ptr<runtime::LogFile> file = storage.open(name);
            std::vector<std::byte> bytes(static_cast<std::size_t>(file->size()));
            static_cast<void>(file->read(0, bytes));
            file->close();
            const auto decoded = runtime::decodeCheckpoint(bytes);
            const auto* checkpoint = std::get_if<runtime::DecodedCheckpoint>(&decoded);
            if (checkpoint == nullptr) {
                throw std::runtime_error("durability: a rolled checkpoint does not decode");
            }
            if (const std::optional<ImageHeader> image = readImageHeader(checkpoint->image)) {
                result.orders += image->orders;
                result.levels = std::max<std::size_t>(result.levels, std::max(image->bidLevels, image->askLevels));
            }
            storage.remove(name);
        }
    }

    // recover: from each book's checkpoint at the first file's end, plus the second file, as a
    // start does: the survey, then the shard's recovery, on a shard built untimed.
    runtime::ShardRoller(storage, instruments, 0, throwOnReport).roll(records);
    ComponentResult recovered{"recover", settings.workload, "checkpoint-and-one-file", result.orders, result.levels, settings.rollPasses, 0,
                              settings.rollPasses * records};
    const std::atomic<bool> running{false};
    for (std::size_t pass = 0; pass < settings.rollPasses; ++pass) {
        runtime::Shard shard(instruments, 0);
        const Clock::time_point start = Clock::now();
        const runtime::LogSurvey survey = runtime::surveyLog(storage, instruments);
        const runtime::RecoveredShard done = runtime::recoverShard(storage, storage, instruments, survey.shards[0], survey.checkpoints[0], shard,
                                                                   runtime::defaultBookCapacity, running);
        recovered.elapsedNanoseconds += nanosecondsBetween(start, Clock::now());
        if (done.start != records || done.replayed != 2 * records) {
            throw std::runtime_error("durability: recovery did not restore at the first file's end and replay the second");
        }
    }
    return {result, recovered};
}

}  // namespace

std::vector<ComponentResult> runDurabilityGroup(const DurabilitySettings& settings) {
    std::vector<ComponentResult> results;
    for (const std::size_t orders : settings.deepBookOrders) {
        results.push_back(publishResult(orders, settings));
    }
    for (ComponentResult& result : rollResults(settings)) {
        results.push_back(std::move(result));
    }
    return results;
}

std::vector<ComponentResult> runImageGroup(const ImageSettings& settings) {
    std::vector<ComponentResult> results;
    for (const std::size_t orders : settings.deepBookOrders) {
        addShapeResults(results, "deep-book", orders, settings);
    }
    addShapeResults(results, "mostly-resting", settings.mostlyRestingOrders, settings);
    return results;
}

double ComponentResult::nanosecondsPerOperation() const {
    return operations == 0 ? 0.0 : static_cast<double>(elapsedNanoseconds) / static_cast<double>(operations);
}

std::string formatComponentCsv(const std::vector<ComponentResult>& results) {
    std::string text = "operation,workload,state,orders,levels,passes,elapsed_ns,ns_per_op\n";
    for (const ComponentResult& result : results) {
        text += result.operation + ',' + result.workload + ',' + result.state + ',' + std::to_string(result.orders) +
                ',' + std::to_string(result.levels) + ',' + std::to_string(result.passes) + ',' +
                std::to_string(result.elapsedNanoseconds) + ',' + formatFixed(result.nanosecondsPerOperation(), 1) +
                '\n';
    }
    return text;
}

std::vector<ComponentResult> runCapacityGroup(const CapacitySettings& settings) {
    std::vector<ComponentResult> results;
    for (const std::size_t orders : settings.orderCounts) {
        results.push_back(growToResult(orders, settings.growthPasses));
    }
    for (const std::size_t orders : settings.orderCounts) {
        results.push_back(reserveResult(orders, settings.growthPasses));
    }
    for (const std::size_t orders : settings.orderCounts) {
        results.push_back(reserveCapacityResult(orders, settings.growthPasses));
    }
    for (const bool far : {false, true}) {
        for (const std::size_t levels : settings.levelCounts) {
            results.push_back(newLevelResult(far, levels, settings.levelPasses));
        }
    }
    return results;
}

std::vector<ComponentResult> runLogGroup(const WorkloadComponentSettings& settings) {
    std::vector<ComponentResult> results;
    for (const std::string& name : settings.workloads) {
        const Workload workload = generateWorkload(findWorkload(name), settings.commands, settings.seed);
        const std::vector<replay::ReplayRecord> records = workloadRecords(workload, componentSymbol());
        results.push_back(logEncodeResult(workload, records, settings.passes));
        results.push_back(readResult<replay::CommandLogReader>("log-read", workload, renderCommandLog(workload, componentSymbol()),
                                                               records.size(), settings.passes));
        results.push_back(readResult<replay::ReplayReader>("csv-read", workload, renderCsv(records), records.size(), settings.passes));
    }
    return results;
}

std::vector<ComponentResult> runEntryGroup(const WorkloadComponentSettings& settings) {
    std::vector<ComponentResult> results;
    results.push_back(symbolCompareResult(settings.passes));
    for (const std::size_t instruments : std::array<std::size_t, 3>{8, 64, 512}) {
        results.push_back(symbolLookupResult(instruments, settings.passes));
    }
    for (const std::string& name : settings.workloads) {
        const Workload workload = generateWorkload(findWorkload(name), settings.commands, settings.seed);
        const std::vector<replay::ReplayRecord> records = workloadRecords(workload, componentSymbol());
        results.push_back(executeCommandResult(workload, records, settings.passes));
        results.push_back(checkEntryResult(workload, records, settings.passes));
    }
    return results;
}

std::vector<ComponentResult> runWireGroup(const WorkloadComponentSettings& settings) {
    std::vector<ComponentResult> results;
    for (const std::string& name : settings.workloads) {
        const Workload workload = generateWorkload(findWorkload(name), settings.commands, settings.seed);
        const std::vector<replay::ReplayRecord> records = workloadRecords(workload, componentSymbol());
        results.push_back(frameDecodeResult(workload, records, settings.passes));
        results.push_back(eventEncodeResult(workload, records, settings.passes));
    }
    return results;
}

std::vector<ComponentResult> runControlGroup(const ControlSettings& settings) {
    if (settings.threads == 0) {
        throw std::invalid_argument("controls need at least one thread");
    }
    std::vector<ComponentResult> results;

    const std::vector<std::int64_t> cpu = runTogether(settings, [&](std::size_t) {
        controlSink = cpuLoop(settings.cpuIterations);
    });
    for (std::size_t thread = 0; thread < settings.threads; ++thread) {
        results.push_back({"cpu-loop", "none", threadLabel(thread, settings.threads), 0, 0, 1, cpu[thread],
                           settings.cpuIterations});
    }

    for (const std::size_t bytes : settings.walkBytes) {
        std::vector<std::vector<std::size_t>> walks;
        // One per thread, so threads share caches and memory but no data.
        for (std::size_t thread = 0; thread < settings.threads; ++thread) {
            walks.push_back(buildWalk(bytes, 1000 + thread));
        }
        const std::vector<std::int64_t> walked = runTogether(settings, [&](std::size_t thread) {
            controlSink = memoryWalk(walks[thread], settings.walkSteps);
        });
        for (std::size_t thread = 0; thread < settings.threads; ++thread) {
            results.push_back({"memory-walk", sizeLabel(bytes), threadLabel(thread, settings.threads), 0, 0, 1,
                               walked[thread], settings.walkSteps});
        }
    }
    return results;
}

}  // namespace order_book::benchmark
