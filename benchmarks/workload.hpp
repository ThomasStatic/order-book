#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "order_book/side.hpp"
#include "order_book/types.hpp"

// Benchmark workloads: what the book is asked to do, decided entirely before anything is
// timed. Generation never touches an OrderBook; it follows its own small model of the book
// (WorkloadModel, in workload.cpp), so a workload depends only on its configuration, size,
// seed, and workloadGeneratorVersion, and the model's prediction of the outcome is an
// independent check on every timed run.
namespace order_book::benchmark {

// Bump whenever a change to the generator or a built-in workload would make some seed produce
// different commands. benchmark_harness_tests pins a fingerprint of each built-in workload, so
// a change that forgets to bump this fails there, and results recorded under one version are
// only comparable with results recorded under the same one.
inline constexpr unsigned int workloadGeneratorVersion = 2;

// How many timed commands a run executes: smoke for a quick check, standard for ordinary
// measurements, extended for long runs. A workload's preload is the same at every size.
enum class BenchmarkSize { SMOKE, STANDARD, EXTENDED };

std::size_t timedCommandCount(BenchmarkSize size);
const char* benchmarkSizeName(BenchmarkSize size);
// Throws std::invalid_argument on a name other than smoke, standard, or extended.
BenchmarkSize parseBenchmarkSize(std::string_view name);

// How a timed submission is priced, relative to the book as the model sees it just before:
// UNIFORM anywhere in its own side's band of levels, CONCENTRATED within a few ticks behind its
// own side's best, MARKETABLE at the opposing best (or a few ticks through it), so it trades.
// Only MARKETABLE orders cross; the others are held one tick behind the opposing best.
enum class PriceMode { UNIFORM, CONCENTRATED, MARKETABLE };

// Quantity ranges, inclusive: small 1-10, medium 11-100, large 101-1000.
enum class QuantityClass { SMALL, MEDIUM, LARGE };

struct QuantityRange {
    Quantity low;
    Quantity high;
};
QuantityRange quantityRange(QuantityClass quantityClass);

// The shape of a workload. Percentages are out of 100, and each mix must sum to 100.
struct WorkloadConfig {
    std::string name;
    std::string objective;

    // The book before timing starts: initialOrders resting orders spread round-robin over
    // targetLevels price levels, alternating bid and ask, one tick apart and away from
    // midPrice. Timed prices stay within that band (see PriceMode), so the level count stays
    // near the target.
    std::size_t initialOrders = 1000;
    unsigned int targetLevels = 100;
    Price midPrice = 100000;

    // Of timed commands, the cancellation rate: cancellations / timed commands. A cancellation
    // drawn while nothing rests becomes a submission instead, so the realised rate can fall
    // below this; WorkloadOutcome reports the realised one.
    unsigned int cancelPercent = 30;
    // Of timed submissions.
    unsigned int buyPercent = 50;
    unsigned int uniformPercent = 60;
    unsigned int concentratedPercent = 30;
    unsigned int marketablePercent = 10;
    Price concentratedTicks = 3;  // CONCENTRATED: 0 to this many ticks behind its own best
    Price throughTicks = 0;       // MARKETABLE: 0 to this many ticks through the opposing best
    // When positive, a submission drawn while at least this many orders rest is priced
    // MARKETABLE whatever its drawn mode, so the book holds near the cap instead of drifting
    // upward. The cancellation rate is unaffected.
    std::size_t activeOrderCap = 0;

    // Of every order, preload included.
    unsigned int smallPercent = 100;
    unsigned int mediumPercent = 0;
    unsigned int largePercent = 0;
};

// Throws std::invalid_argument, naming the workload and the problem, for a configuration that
// cannot be generated: a mix that does not sum to 100, fewer than 2 levels, fewer initial orders than
// levels, or a band that does not fit between price 1 and the largest Price.
void validateWorkloadConfig(const WorkloadConfig& config);

// The five built-in workloads, in a fixed order: small-book, deep-book, matching-heavy,
// cancellation-heavy, mostly-resting.
const std::vector<WorkloadConfig>& builtInWorkloads();
// Throws std::invalid_argument, listing the known names, for an unknown one.
const WorkloadConfig& findWorkload(std::string_view name);

enum class CommandType { SUBMIT, CANCEL };

// One command to the book: a limit order to submit (every field used) or an order ID to
// cancel (only orderId used; the rest stay zero).
struct BenchmarkCommand {
    CommandType type;
    OrderId orderId;
    Side side;
    Price price;
    Quantity quantity;

    bool operator==(const BenchmarkCommand&) const = default;
};

// What the timed commands do, as the model predicts it for the generator and as a runner
// counts it from the book's results. The two must agree on every run.
struct WorkloadOutcome {
    std::size_t submits = 0;
    std::size_t cancels = 0;
    std::size_t rejections = 0;   // always 0 for a generated workload
    std::size_t trades = 0;
    std::uint64_t volume = 0;     // the sum of every trade's quantity
    std::size_t finalActiveOrders = 0;
    std::size_t finalPriceLevels = 0;  // both sides

    // cancels / (submits + cancels), or 0 for no commands.
    double cancellationRate() const;

    bool operator==(const WorkloadOutcome&) const = default;
};

struct Workload {
    WorkloadConfig config;
    std::uint64_t seed = 0;
    // Applied, untimed, to a fresh book before each run; none of them trade.
    std::vector<BenchmarkCommand> preload;
    // Run, timed, against the preloaded book; every one is accepted.
    std::vector<BenchmarkCommand> timed;
    WorkloadOutcome expected;
};

// The preload and timedCommands timed commands for config and seed. The same arguments give
// the same workload on every platform: draws come from std::mt19937_64's raw output, never a
// standard distribution, each in its own statement. Order IDs are fresh for every order and
// never reused. Throws as validateWorkloadConfig does, or std::length_error if the IDs would
// pass the largest OrderId.
Workload generateWorkload(const WorkloadConfig& config, std::size_t timedCommands, std::uint64_t seed);

// FNV-1a over every command (preload, then timed) and the expected outcome, for pinning a
// workload in tests and labelling recorded results.
std::uint64_t workloadFingerprint(const Workload& workload);

}  // namespace order_book::benchmark
