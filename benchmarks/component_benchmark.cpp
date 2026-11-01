#include "component_benchmark.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <latch>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include "benchmark_report.hpp"
#include "benchmark_runner.hpp"
#include "order_book/command.hpp"
#include "order_book/command_log.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#include "order_book/order_index.hpp"
#include "order_book/replay_parser.hpp"
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

}  // namespace

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
    for (const std::string& name : settings.workloads) {
        const Workload workload = generateWorkload(findWorkload(name), settings.commands, settings.seed);
        results.push_back(executeCommandResult(workload, workloadRecords(workload, componentSymbol()), settings.passes));
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
