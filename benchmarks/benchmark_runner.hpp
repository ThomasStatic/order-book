#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "order_book/order_book.hpp"
#include "order_book/replay_parser.hpp"
#include "order_book/runtime/matching_loop.hpp"
#include "order_book/runtime/shard.hpp"
#include "workload.hpp"

// Runs generated workloads against the book and times them. Only the timed commands are
// inside the timed region: generating the workload, building the book, preloading it,
// rendering CSV text, checking the outcome, and destroying the book all happen outside it.
namespace order_book::benchmark {

// CORE times the engine alone: each command goes straight to OrderBook::submitOrder or
// cancelOrder, and no CSV is involved anywhere. REPLAY times the end-to-end path of
// docs/replay-format.md from text already in memory: parsing, the replay engine's dispatch
// and event collection, the final snapshot, and the formatted summary. Disk I/O is left out
// of both, since it would mostly measure the file cache.
// REPLAY_LOG is REPLAY's path with the same commands as a binary command log in memory, so the
// difference between the two is the cost of the text.
// SHARD runs the commands through Shard::process on one thread, with no rings, so its difference
// from CORE is Command decoding, BookSet, and result reuse.
// QUEUE puts real rings and a real matching thread between a producer thread and that shard, so
// its difference from SHARD is the hand-off.
enum class BenchmarkKind { CORE, REPLAY, REPLAY_LOG, SHARD, QUEUE };

const char* benchmarkKindName(BenchmarkKind kind);
// Throws std::invalid_argument on a name other than core, replay, replay-log, shard, or queue.
BenchmarkKind parseBenchmarkKind(std::string_view name);

// One timed run of a workload's timed commands on a freshly preloaded book.
struct Measurement {
    std::string workload;
    BenchmarkKind kind = BenchmarkKind::CORE;
    // Counted from 1, separately for warm-ups and measured repetitions.
    std::size_t repetition = 0;
    // A warm-up's time is kept with the rest but left out of every summary.
    bool warmup = false;
    std::size_t commands = 0;
    std::size_t instruments = 1;  // 1 but for a SHARD run of several
    std::int64_t elapsedNanoseconds = 0;
    // What the book did, counted from its results; it always equals the workload's expected
    // outcome, or the run throws instead of returning a measurement.
    WorkloadOutcome outcome;

    double operationsPerSecond() const;
    double nanosecondsPerCommand() const;
};

// commands / elapsed seconds; 0 when nothing was timed.
double operationsPerSecond(std::size_t commands, std::int64_t elapsedNanoseconds);

struct RunSettings {
    std::size_t warmups = 1;
    std::size_t repetitions = 5;
};

// The capacity each run reserves before its preload, untimed: the most the book holds at any
// moment by the workload's model, so a run reaches the heap only if the prediction is wrong,
// and any capacityGrowths fails it. levels has one to spare for a replace, which makes its new
// level before it leaves its old one. It is recorded with every result (JSON "reserved_orders",
// "reserved_stops", "reserved_levels").
BookCapacity expectedBookSize(const Workload& workload);

// Reserves the book and applies the workload's preload. Throws std::runtime_error if the book
// refuses or trades any of it.
void preloadBook(OrderBook& book, const Workload& workload);
// Throws std::runtime_error if the book grew past its capacity: the run's capacity was wrong.
void requireNoGrowth(const OrderBook& book, const Workload& workload);
// Adds the book's final shape to an outcome, for comparing with the workload's prediction.
void recordFinalBook(const OrderBook& book, WorkloadOutcome& outcome);
// One line of every counter, for a failure message.
std::string describeOutcome(const WorkloadOutcome& outcome);

// Times the workload's commands once on a fresh book, preloaded outside the timed region.
// Throws std::runtime_error, describing both, if the book's outcome differs from the one the
// workload's model predicted, so a measurement never stands for work the book did not do, or if
// the book grew past its capacity.
Measurement runOnce(const Workload& workload, BenchmarkKind kind);

// settings.warmups runs, then settings.repetitions measured runs, each on its own fresh book,
// in that order. Every run is returned, warm-ups first.
std::vector<Measurement> runRepeated(const Workload& workload, BenchmarkKind kind, const RunSettings& settings);

// One workload per instrument, sharing timedCommands between them, each its own unchanged
// generateWorkload stream. Instrument 0 uses seed, so one instrument is exactly today's stream,
// and no fingerprint moves.
std::vector<Workload> instrumentWorkloads(const WorkloadConfig& config, std::size_t timedCommands, std::uint64_t seed,
                                          std::size_t instruments);

// What these workloads predict between them, counter by counter.
WorkloadOutcome totalOutcome(const std::vector<Workload>& workloads);

// Throws std::runtime_error if any book's outcome differs from its workload's prediction.
void requireOutcomes(const std::vector<WorkloadOutcome>& outcomes, const std::vector<Workload>& workloads, BenchmarkKind kind);

// One Shard on this thread, holding a book for each workload, warmed to that workload's peak and
// preloaded. run is what a timed region holds: it makes no heap call for a book inside its
// capacity. An interleaver seeded from the first workload's seed shares the commands out, each
// instrument keeping its own order, so each book's prediction holds. The workloads must outlive
// this.
class ShardRun {
public:
    explicit ShardRun(const std::vector<Workload>& workloads);
    ~ShardRun();

    void run();

    runtime::Shard& onThisThread() noexcept { return *shard; }
    // The book that takes each command, in the order they go, and a book's slot and commands.
    const std::vector<std::uint32_t>& sharing() const noexcept { return order; }
    std::uint32_t slotOf(std::uint32_t book) const;
    const replay::Command& commandOf(std::uint32_t book, std::size_t index) const;

    // The books' final shapes added to what run counted, per workload.
    std::vector<WorkloadOutcome> outcomes() const;
    // Throws std::runtime_error if a book did other than its workload predicted, or grew.
    void check() const;

private:
    struct Book;

    const std::vector<Workload>* workloads;
    std::unique_ptr<order_book::runtime::InstrumentDirectory> directory;
    std::unique_ptr<order_book::runtime::Shard> shard;
    std::vector<Book> books;
    std::vector<std::uint32_t> order;  // which book takes each command
};

// A real matching thread, which builds and preloads a ShardRun's books and so owns them, with this
// thread as its one producer. Events are counted here, and final shapes are read on the matching
// thread once it has stopped. The workloads must outlive this.
class QueueRun {
public:
    // setUpMatchingThread, if given, runs first on the matching thread, for pinning it.
    explicit QueueRun(const std::vector<Workload>& workloads, std::function<void()> setUpMatchingThread = {});
    // Stops the matching thread if finish has not.
    ~QueueRun();

    // Every command through the rings, sending and draining at once. What a timed region holds.
    void run();

    // One command at a time, for a latency target. prepare builds the element and is outside
    // the interval. awaitResponse returns once its last event is back.
    void prepare(std::size_t index);
    void sendPrepared();
    void awaitResponse();

    // Stops the matching thread and collects the books' final shapes. Rethrows anything it threw.
    void finish();
    // After finish. Throws std::runtime_error if a book did other than its workload predicted.
    void check() const;
    std::vector<WorkloadOutcome> outcomes() const;

private:
    bool trySend(const runtime::CommandElement& element);
    void drainResponses();
    runtime::CommandElement elementFor(std::uint32_t book);

    const std::vector<Workload>* workloads;
    runtime::Doorbell doorbell;
    runtime::Ring<runtime::CommandElement> commands;
    runtime::Ring<runtime::ResponseRecord> responses;
    const ShardRun* books = nullptr;  // on the matching thread, until it stops
    std::vector<std::size_t> nextCommand;
    std::vector<WorkloadOutcome> counted;
    std::vector<WorkloadOutcome> finalShapes;
    std::optional<runtime::CommandElement> held;
    std::size_t sent = 0;
    std::size_t answered = 0;
    bool stopReceived = false;
    std::exception_ptr failure;
    std::thread matching;
};

// runOnce for a SHARD or QUEUE run of several instruments, whose outcome is the run's total.
Measurement runInstrumentsOnce(const std::vector<Workload>& workloads, BenchmarkKind kind);
// runRepeated for the same.
std::vector<Measurement> runInstrumentsRepeated(const std::vector<Workload>& workloads, BenchmarkKind kind,
                                                const RunSettings& settings);

// The workload's timed commands as a replay file (header, then one record per command,
// numbered from 1), for the REPLAY kind and for inspecting a workload by hand.
std::string renderReplayCsv(const std::vector<BenchmarkCommand>& commands);
// A behavior workload's commands as v2 replay rows, under the v2 header.
std::string renderReplayCsv(const std::vector<BehaviorCommand>& commands);

// The workload's timed commands as the replay records they are, naming no instrument's book.
std::vector<order_book::replay::ReplayRecord> workloadRecords(const Workload& workload, order_book::replay::Symbol symbol);
// The same for its preload.
std::vector<order_book::replay::ReplayRecord> preloadRecords(const Workload& workload, order_book::replay::Symbol symbol);
// The same as a command log in memory, with no placement at position 0.
std::string renderCommandLog(const Workload& workload, order_book::replay::Symbol symbol);

}  // namespace order_book::benchmark
