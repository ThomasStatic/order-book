#include "benchmark_runner.hpp"

#include <chrono>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <variant>

#include "order_book/cancellation_results.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#include "order_book/replay_engine.hpp"
#include "order_book/replay_summary.hpp"
#include "order_book/submission_results.hpp"

namespace order_book::benchmark {

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t nanosecondsBetween(Clock::time_point start, Clock::time_point stop) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
}

// Sends one command to the book and counts what it did. The sequence number passed to Order
// is a placeholder: the book assigns its own on acceptance.
void execute(OrderBook& book, const BenchmarkCommand& command, WorkloadOutcome& outcome) {
    if (command.type == CommandType::SUBMIT) {
        ++outcome.submits;
        const SubmissionResults result =
            book.submitOrder(Order(command.orderId, command.price, command.quantity, 1, command.side));
        if (result.getStatus() == SubmissionStatus::REJECTED) {
            ++outcome.rejections;
        }
        for (const Trade& trade : result.getGeneratedTrades()) {
            ++outcome.trades;
            outcome.volume += trade.getExecutionQuantity();
        }
    } else {
        ++outcome.cancels;
        if (!book.cancelOrder(command.orderId).isSuccess()) {
            ++outcome.rejections;
        }
    }
}

void preload(OrderBook& book, const Workload& workload) {
    WorkloadOutcome preloadOutcome;
    for (const BenchmarkCommand& command : workload.preload) {
        execute(book, command, preloadOutcome);
    }
    if (preloadOutcome.rejections != 0 || preloadOutcome.trades != 0) {
        throw std::runtime_error("workload " + workload.config.name + ": the book refused or traded its preload");
    }
}

void recordFinalBook(const OrderBook& book, WorkloadOutcome& outcome) {
    const BookSnapshot snapshot = book.snapshot(std::numeric_limits<unsigned int>::max());
    outcome.finalActiveOrders = book.activeOrderCount();
    outcome.finalPriceLevels = snapshot.bids.size() + snapshot.asks.size();
}

std::string describeOutcome(const WorkloadOutcome& outcome) {
    return "submits=" + std::to_string(outcome.submits) + " cancels=" + std::to_string(outcome.cancels) +
           " rejections=" + std::to_string(outcome.rejections) + " trades=" + std::to_string(outcome.trades) +
           " volume=" + std::to_string(outcome.volume) + " active=" + std::to_string(outcome.finalActiveOrders) +
           " levels=" + std::to_string(outcome.finalPriceLevels);
}

Measurement runCore(const Workload& workload) {
    Measurement measurement;
    OrderBook book;
    preload(book, workload);

    const Clock::time_point start = Clock::now();
    for (const BenchmarkCommand& command : workload.timed) {
        execute(book, command, measurement.outcome);
    }
    const Clock::time_point stop = Clock::now();

    measurement.elapsedNanoseconds = nanosecondsBetween(start, stop);
    recordFinalBook(book, measurement.outcome);
    return measurement;
}

Measurement runReplay(const Workload& workload) {
    namespace replay = order_book::replay;

    Measurement measurement;
    OrderBook book;
    preload(book, workload);
    // Built outside the timed region: the text stands in for a file already read into memory.
    std::istringstream csv(renderReplayCsv(workload.timed));
    replay::ReplayOptions options;
    options.invariantValidation = replay::InvariantValidation::NONE;

    const Clock::time_point start = Clock::now();
    const replay::ReplayInput input = replay::readReplayInput(csv);
    const replay::ReplayOutcome outcome = replay::ReplayEngine::run(input.records, book, options);
    const replay::ReplayResult* result = std::get_if<replay::ReplayResult>(&outcome);
    const std::string summary = result != nullptr ? replay::formatReplaySummary(*result) : std::string();
    const Clock::time_point stop = Clock::now();

    measurement.elapsedNanoseconds = nanosecondsBetween(start, stop);
    if (input.error || result == nullptr || summary.empty()) {
        throw std::runtime_error("workload " + workload.config.name + ": the replay of its commands failed");
    }
    const replay::ReplayStatistics& statistics = result->statistics;
    measurement.outcome.submits = statistics.addCommands;
    measurement.outcome.cancels = statistics.cancelCommands;
    measurement.outcome.rejections = statistics.addsRejected + statistics.cancelsRejected;
    measurement.outcome.trades = statistics.tradesExecuted;
    measurement.outcome.volume = statistics.volumeTraded;
    measurement.outcome.finalActiveOrders = result->finalBook.totalActiveVisibleOrders;
    measurement.outcome.finalPriceLevels = result->finalBook.bids.size() + result->finalBook.asks.size();
    return measurement;
}

}  // namespace

const char* benchmarkKindName(BenchmarkKind kind) {
    switch (kind) {
        case BenchmarkKind::CORE: return "core";
        case BenchmarkKind::REPLAY: return "replay";
    }
    return "unknown";
}

BenchmarkKind parseBenchmarkKind(std::string_view name) {
    for (const BenchmarkKind kind : {BenchmarkKind::CORE, BenchmarkKind::REPLAY}) {
        if (name == benchmarkKindName(kind)) {
            return kind;
        }
    }
    throw std::invalid_argument("unknown kind '" + std::string(name) + "' (known: core, replay)");
}

double operationsPerSecond(std::size_t commands, std::int64_t elapsedNanoseconds) {
    if (elapsedNanoseconds <= 0) {
        return 0.0;
    }
    return static_cast<double>(commands) * 1e9 / static_cast<double>(elapsedNanoseconds);
}

double Measurement::operationsPerSecond() const {
    return benchmark::operationsPerSecond(commands, elapsedNanoseconds);
}

double Measurement::nanosecondsPerCommand() const {
    return commands == 0 ? 0.0 : static_cast<double>(elapsedNanoseconds) / static_cast<double>(commands);
}

Measurement runOnce(const Workload& workload, BenchmarkKind kind) {
    Measurement measurement = kind == BenchmarkKind::CORE ? runCore(workload) : runReplay(workload);
    measurement.workload = workload.config.name;
    measurement.kind = kind;
    measurement.commands = workload.timed.size();
    if (measurement.outcome != workload.expected) {
        throw std::runtime_error("workload " + workload.config.name + " (" + benchmarkKindName(kind) +
                                 "): the book did " + describeOutcome(measurement.outcome) +
                                 " but the workload predicted " + describeOutcome(workload.expected));
    }
    return measurement;
}

std::vector<Measurement> runRepeated(const Workload& workload, BenchmarkKind kind, const RunSettings& settings) {
    std::vector<Measurement> measurements;
    measurements.reserve(settings.warmups + settings.repetitions);
    for (std::size_t i = 0; i < settings.warmups + settings.repetitions; ++i) {
        Measurement measurement = runOnce(workload, kind);
        measurement.warmup = i < settings.warmups;
        measurement.repetition = measurement.warmup ? i + 1 : i - settings.warmups + 1;
        measurements.push_back(std::move(measurement));
    }
    return measurements;
}

std::string renderReplayCsv(const std::vector<BenchmarkCommand>& commands) {
    std::string csv(order_book::replay::expectedHeader);
    csv += '\n';
    std::uint64_t sequence = 0;
    for (const BenchmarkCommand& command : commands) {
        csv += std::to_string(++sequence);
        if (command.type == CommandType::SUBMIT) {
            csv += ",ADD,";
            csv += std::to_string(command.orderId);
            csv += command.side == Side::BUY ? ",BUY," : ",SELL,";
            csv += std::to_string(command.price);
            csv += ',';
            csv += std::to_string(command.quantity);
        } else {
            csv += ",CANCEL,";
            csv += std::to_string(command.orderId);
            csv += ",,,";
        }
        csv += '\n';
    }
    return csv;
}

}  // namespace order_book::benchmark
