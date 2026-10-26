// Counts heap allocations on the benchmark workloads, exactly rather than by sampling.
//
// Global operator new and delete are replaced with counting versions, then each built-in
// workload is generated as the benchmark does and run once on a preloaded book. For the
// core path every timed command is run on its own and its allocations, frees, and bytes are
// charged to its kind: a submission that rests without trading, one that trades and rests
// its remainder, one that fills completely, or a cancellation. A histogram of allocation
// sizes per kind says which container each allocation came from (a std::list node, an index
// node, a map node, a result's events vector, a trades vector), since each has a distinct
// size. The order index (std::unordered_map) is watched for rehashes, and its bucket count
// and load factor are reported at the end.
//
// For the replay path the three stages the benchmark times (parsing the CSV text, running
// the records, formatting the summary) are counted and timed separately, so the cost of the
// replay layers can be split between them.
//
// Usage: allocation_profile [--size smoke|standard|extended] [--seed N] [--workload NAME]...
// Prints to stdout; run it from a Release build, since a Debug build's checked iterators
// and automatic invariant checks allocate and would be counted too.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "benchmark_runner.hpp"
#include "order_book/cancellation_results.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#include "order_book/replay_engine.hpp"
#include "order_book/replay_parser.hpp"
#include "order_book/replay_summary.hpp"
#include "order_book/submission_results.hpp"
#include "workload.hpp"

namespace {

constexpr std::size_t histogramLimit = 1024;

struct AllocationCounts {
    std::size_t allocations = 0;
    std::size_t frees = 0;
    std::size_t bytes = 0;
};

AllocationCounts counts;
// Sizes below histogramLimit, exactly; larger ones go in the last slot.
std::size_t sizeHistogram[histogramLimit + 1];

void countAllocation(std::size_t size) {
    ++counts.allocations;
    counts.bytes += size;
    ++sizeHistogram[size < histogramLimit ? size : histogramLimit];
}

}  // namespace

void* operator new(std::size_t size) {
    countAllocation(size);
    if (void* block = std::malloc(size == 0 ? 1 : size)) {
        return block;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return operator new(size);
}

void operator delete(void* block) noexcept {
    if (block != nullptr) {
        ++counts.frees;
    }
    std::free(block);
}

void operator delete(void* block, std::size_t) noexcept {
    operator delete(block);
}

void operator delete[](void* block) noexcept {
    operator delete(block);
}

void operator delete[](void* block, std::size_t) noexcept {
    operator delete(block);
}

namespace order_book {

// The friend the engine's headers declare for tests; here it reads the index's hash table.
struct OrderBookTestAccess {
    static std::size_t bucketCount(const OrderBook& book) { return book.orderIndex.bucket_count(); }
    static float loadFactor(const OrderBook& book) { return book.orderIndex.load_factor(); }
    static float maxLoadFactor(const OrderBook& book) { return book.orderIndex.max_load_factor(); }
    static std::size_t levelCount(const OrderBook& book) { return book.bids.size() + book.asks.size(); }
    static std::size_t listNodeSize() {
#ifdef _MSC_VER
        return sizeof(std::_List_node<Order, void*>);
#else
        return sizeof(Order) + 2 * sizeof(void*);  // libstdc++ and libc++: two links then the value
#endif
    }
};

}  // namespace order_book

namespace {

using namespace order_book;
using namespace order_book::benchmark;
using Clock = std::chrono::steady_clock;

enum class CommandKind { SUBMIT_RESTS, SUBMIT_TRADES_AND_RESTS, SUBMIT_FILLED, CANCEL, KINDS };

const char* commandKindName(CommandKind kind) {
    switch (kind) {
        case CommandKind::SUBMIT_RESTS: return "submit, rests without trading";
        case CommandKind::SUBMIT_TRADES_AND_RESTS: return "submit, trades then rests the remainder";
        case CommandKind::SUBMIT_FILLED: return "submit, filled completely";
        case CommandKind::CANCEL: return "cancel";
        case CommandKind::KINDS: break;
    }
    return "?";
}

struct KindTotals {
    std::size_t commands = 0;
    std::size_t trades = 0;
    AllocationCounts allocations;
    std::size_t histogram[histogramLimit + 1] = {};
};

AllocationCounts snapshotCounts() {
    return counts;
}

AllocationCounts difference(const AllocationCounts& before, const AllocationCounts& after) {
    return {after.allocations - before.allocations, after.frees - before.frees, after.bytes - before.bytes};
}

void accumulate(AllocationCounts& into, const AllocationCounts& delta) {
    into.allocations += delta.allocations;
    into.frees += delta.frees;
    into.bytes += delta.bytes;
}

std::string fixed(double value, int decimals) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    return buffer;
}

double per(std::size_t total, std::size_t commands) {
    return commands == 0 ? 0.0 : static_cast<double>(total) / static_cast<double>(commands);
}

void printHistogram(const std::size_t* histogram, std::size_t commands, const char* indent) {
    std::vector<std::pair<std::size_t, std::size_t>> rows;
    for (std::size_t size = 0; size <= histogramLimit; ++size) {
        if (histogram[size] != 0) {
            rows.emplace_back(histogram[size], size);
        }
    }
    std::sort(rows.rbegin(), rows.rend());
    std::size_t shown = 0;
    for (const auto& [count, size] : rows) {
        if (shown++ == 12) {
            std::printf("%s  ... %zu more sizes\n", indent, rows.size() - 12);
            break;
        }
        if (size == histogramLimit) {
            std::printf("%s  >= %zu bytes: %zu allocations (%s per command)\n", indent, histogramLimit, count,
                        fixed(per(count, commands), 3).c_str());
        } else {
            std::printf("%s  %zu bytes: %zu allocations (%s per command)\n", indent, size, count,
                        fixed(per(count, commands), 3).c_str());
        }
    }
}

void profileCore(const Workload& workload) {
    OrderBook book;
    book.reserve(expectedBookSize(workload));
    std::size_t preloadRehashes = 0;
    const AllocationCounts beforePreload = snapshotCounts();
    for (const BenchmarkCommand& command : workload.preload) {
        const std::size_t buckets = OrderBookTestAccess::bucketCount(book);
        book.submitOrder(Order(command.orderId, command.price, command.quantity, 1, command.side));
        if (OrderBookTestAccess::bucketCount(book) != buckets) {
            ++preloadRehashes;
        }
    }
    const AllocationCounts preloadCounts = difference(beforePreload, snapshotCounts());
    std::printf("  preload: %zu orders, %zu allocations (%s per order), %zu bytes, %zu index rehashes; "
                "%zu buckets, load factor %s\n",
                workload.preload.size(), preloadCounts.allocations, fixed(per(preloadCounts.allocations, workload.preload.size()), 3).c_str(),
                preloadCounts.bytes, preloadRehashes, OrderBookTestAccess::bucketCount(book),
                fixed(OrderBookTestAccess::loadFactor(book), 3).c_str());

    KindTotals totals[static_cast<std::size_t>(CommandKind::KINDS)];
    std::size_t rehashes = 0;
    std::size_t levelsCreated = 0;
    std::size_t levelsErased = 0;
    std::size_t maxBuckets = OrderBookTestAccess::bucketCount(book);
    const AllocationCounts beforeTimed = snapshotCounts();
    for (const BenchmarkCommand& command : workload.timed) {
        const std::size_t buckets = OrderBookTestAccess::bucketCount(book);
        const std::size_t levels = OrderBookTestAccess::levelCount(book);
        CommandKind kind = CommandKind::CANCEL;
        std::size_t trades = 0;
        std::size_t histogramBefore[histogramLimit + 1];
        std::copy(sizeHistogram, sizeHistogram + histogramLimit + 1, histogramBefore);
        const AllocationCounts before = snapshotCounts();
        if (command.type == CommandType::SUBMIT) {
            const SubmissionResults result =
                book.submitOrder(Order(command.orderId, command.price, command.quantity, 1, command.side));
            trades = result.getGeneratedTrades().size();
            kind = trades == 0 ? CommandKind::SUBMIT_RESTS
                 : result.getRemainingQuantity() == 0 ? CommandKind::SUBMIT_FILLED
                                                      : CommandKind::SUBMIT_TRADES_AND_RESTS;
        } else {
            book.cancelOrder(command.orderId);
        }
        const AllocationCounts delta = difference(before, snapshotCounts());
        KindTotals& total = totals[static_cast<std::size_t>(kind)];
        ++total.commands;
        total.trades += trades;
        accumulate(total.allocations, delta);
        for (std::size_t size = 0; size <= histogramLimit; ++size) {
            total.histogram[size] += sizeHistogram[size] - histogramBefore[size];
        }
        const std::size_t bucketsAfter = OrderBookTestAccess::bucketCount(book);
        if (bucketsAfter != buckets) {
            ++rehashes;
            maxBuckets = std::max(maxBuckets, bucketsAfter);
        }
        const std::size_t levelsAfter = OrderBookTestAccess::levelCount(book);
        if (levelsAfter > levels) {
            levelsCreated += levelsAfter - levels;
        } else {
            levelsErased += levels - levelsAfter;
        }
    }
    const AllocationCounts timedCounts = difference(beforeTimed, snapshotCounts());
    std::printf("  timed: %zu commands, %zu allocations (%s per command), %zu frees (%s per command), %zu bytes "
                "(%s per command)\n",
                workload.timed.size(), timedCounts.allocations, fixed(per(timedCounts.allocations, workload.timed.size()), 3).c_str(),
                timedCounts.frees, fixed(per(timedCounts.frees, workload.timed.size()), 3).c_str(), timedCounts.bytes,
                fixed(per(timedCounts.bytes, workload.timed.size()), 1).c_str());
    std::printf("  order index: %zu rehashes during the timed commands, %zu buckets at most, %zu at the end, load "
                "factor %s (max %s), %zu resting orders\n",
                rehashes, maxBuckets, OrderBookTestAccess::bucketCount(book),
                fixed(OrderBookTestAccess::loadFactor(book), 3).c_str(), fixed(OrderBookTestAccess::maxLoadFactor(book), 1).c_str(),
                book.activeOrderCount());
    std::printf("  price levels: %zu created and %zu erased during the timed commands, %zu at the end\n", levelsCreated,
                levelsErased, OrderBookTestAccess::levelCount(book));
    for (std::size_t k = 0; k < static_cast<std::size_t>(CommandKind::KINDS); ++k) {
        const KindTotals& total = totals[k];
        if (total.commands == 0) {
            continue;
        }
        std::printf("  %s: %zu commands", commandKindName(static_cast<CommandKind>(k)), total.commands);
        if (total.trades != 0) {
            std::printf(", %zu trades (%s per command)", total.trades, fixed(per(total.trades, total.commands), 2).c_str());
        }
        std::printf("\n    %s allocations, %s frees, %s bytes per command\n",
                    fixed(per(total.allocations.allocations, total.commands), 3).c_str(),
                    fixed(per(total.allocations.frees, total.commands), 3).c_str(),
                    fixed(per(total.allocations.bytes, total.commands), 1).c_str());
        printHistogram(total.histogram, total.commands, "  ");
    }
}

void profileReplay(const Workload& workload) {
    OrderBook book;
    book.reserve(expectedBookSize(workload));
    for (const BenchmarkCommand& command : workload.preload) {
        book.submitOrder(Order(command.orderId, command.price, command.quantity, 1, command.side));
    }
    const std::string csv = renderReplayCsv(workload.timed);
    std::istringstream input(csv);
    replay::ReplayOptions options;
    options.invariantValidation = replay::InvariantValidation::NONE;
    const std::size_t commands = workload.timed.size();

    std::size_t histogramBefore[histogramLimit + 1];
    std::copy(sizeHistogram, sizeHistogram + histogramLimit + 1, histogramBefore);
    AllocationCounts before = snapshotCounts();
    Clock::time_point start = Clock::now();
    const replay::ReplayInput replayInput = replay::readReplayInput(input);
    Clock::time_point stop = Clock::now();
    AllocationCounts parse = difference(before, snapshotCounts());
    const auto parseNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
    std::size_t parseHistogram[histogramLimit + 1];
    for (std::size_t size = 0; size <= histogramLimit; ++size) {
        parseHistogram[size] = sizeHistogram[size] - histogramBefore[size];
    }

    before = snapshotCounts();
    start = Clock::now();
    const replay::ReplayOutcome outcome = replay::ReplayEngine::run(replayInput.records, book, options);
    stop = Clock::now();
    AllocationCounts run = difference(before, snapshotCounts());
    const auto runNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
    const replay::ReplayResult* result = std::get_if<replay::ReplayResult>(&outcome);
    if (result == nullptr) {
        std::printf("  replay failed\n");
        return;
    }

    before = snapshotCounts();
    start = Clock::now();
    const std::string summary = replay::formatReplaySummary(*result);
    stop = Clock::now();
    AllocationCounts format = difference(before, snapshotCounts());
    const auto formatNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();

    std::printf("  csv text: %zu bytes (%s per command); %zu records parsed; %zu events collected (%s per command); "
                "summary %zu bytes\n",
                csv.size(), fixed(per(csv.size(), commands), 1).c_str(), replayInput.records.size(), result->events.size(),
                fixed(per(result->events.size(), commands), 3).c_str(), summary.size());
    std::printf("  sizes: ReplayRecord %zu, CsvRow %zu, ReplayEvent %zu, BookEvent %zu, ParseError %zu bytes\n",
                sizeof(replay::ReplayRecord), sizeof(replay::CsvRow), sizeof(replay::ReplayEvent), sizeof(BookEvent),
                sizeof(replay::ParseError));
    std::printf("  parse (readReplayInput): %s ns per command (one run, not a benchmark); %s allocations, %s frees, %s "
                "bytes per command\n",
                fixed(per(static_cast<std::size_t>(parseNanoseconds), commands), 0).c_str(),
                fixed(per(parse.allocations, commands), 3).c_str(), fixed(per(parse.frees, commands), 3).c_str(),
                fixed(per(parse.bytes, commands), 1).c_str());
    printHistogram(parseHistogram, commands, "  ");
    std::printf("  run (ReplayEngine::run, book commands included): %s ns per command; %s allocations, %s frees, %s "
                "bytes per command\n",
                fixed(per(static_cast<std::size_t>(runNanoseconds), commands), 0).c_str(),
                fixed(per(run.allocations, commands), 3).c_str(), fixed(per(run.frees, commands), 3).c_str(),
                fixed(per(run.bytes, commands), 1).c_str());
    std::printf("  format (formatReplaySummary): %s ns per command; %s allocations, %s bytes per command\n",
                fixed(per(static_cast<std::size_t>(formatNanoseconds), commands), 0).c_str(),
                fixed(per(format.allocations, commands), 3).c_str(), fixed(per(format.bytes, commands), 1).c_str());
}

}  // namespace

int main(int argc, char** argv) {
    BenchmarkSize size = BenchmarkSize::STANDARD;
    std::uint64_t seed = 42;
    std::vector<std::string> names;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (i + 1 >= argc) {
            std::fprintf(stderr, "option %s needs a value\n", argv[i]);
            return 2;
        }
        const std::string value = argv[++i];
        if (argument == "--size") {
            size = parseBenchmarkSize(value);
        } else if (argument == "--seed") {
            seed = std::stoull(value);
        } else if (argument == "--workload") {
            names.push_back(value);
        } else {
            std::fprintf(stderr, "unknown option %s\n", argv[i - 1]);
            return 2;
        }
    }

    std::printf("# Allocation profile\n\nsize %s (%zu timed commands), seed %llu, generator version %u\n",
                benchmarkSizeName(size), timedCommandCount(size), static_cast<unsigned long long>(seed),
                workloadGeneratorVersion);
    std::printf("sizes: Order %zu, std::list node of Order %zu, PriceLevel %zu, OrderLocation %zu, Trade %zu, "
                "BookEvent %zu, SubmissionResults %zu, CancellationResult %zu bytes\n\n",
                sizeof(Order), OrderBookTestAccess::listNodeSize(), sizeof(PriceLevel), sizeof(OrderLocation), sizeof(Trade),
                sizeof(BookEvent), sizeof(SubmissionResults), sizeof(CancellationResult));

    std::vector<WorkloadConfig> configs;
    if (names.empty()) {
        configs = builtInWorkloads();
    } else {
        for (const std::string& name : names) {
            configs.push_back(findWorkload(name));
        }
    }
    for (const WorkloadConfig& config : configs) {
        const Workload workload = generateWorkload(config, timedCommandCount(size), seed);
        std::printf("## %s, core\n\n", config.name.c_str());
        profileCore(workload);
        std::printf("\n## %s, replay\n\n", config.name.c_str());
        profileReplay(workload);
        std::printf("\n");
    }
    return 0;
}
