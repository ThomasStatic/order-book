#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "order_book/order_request.hpp"
#include "order_book/replace_request.hpp"
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
inline constexpr unsigned int workloadGeneratorVersion = 3;

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

// Which commands a workload is made of. V1 workloads (the five built-in ones) are compact
// BenchmarkCommands run through submitOrder(Order) and cancelOrder, so their numbers measure v1's
// path unchanged. BEHAVIOR workloads are BehaviorCommands, run through submitOrder(OrderRequest),
// replaceOrder, and cancelOrder, generated and predicted with the randomized harness's
// ReferenceBook (tests/reference/).
enum class WorkloadFamily { V1, BEHAVIOR };

// What a behavior workload draws, per timed command. Each mix sums to 100. Prices follow the
// v1 configuration's band: passive orders within a few ticks behind their own best, marketable
// ones at the opposing best or through it.
struct BehaviorMix {
    // The v1 workload whose exact stream this one replays through the request path (owner 0,
    // or with ownerPerOrder a distinct nonzero owner on every order); empty for none.
    std::string replaysWorkload;
    bool ownerPerOrder = false;

    // Of timed commands: submissions, replaces, cancels of resting orders, new pending stops,
    // and cancels of pending stops. A command whose target does not exist becomes a submission.
    unsigned int submitPercent = 100;
    unsigned int replacePercent = 0;
    unsigned int cancelPercent = 0;
    unsigned int stopPercent = 0;
    unsigned int cancelStopPercent = 0;

    // Of submissions: passive GTC limits, marketable GTC limits, IOC limits, FOK limits, market
    // orders (IOC), and large marketable orders that move the price through several levels.
    unsigned int passivePercent = 100;
    unsigned int marketablePercent = 0;
    unsigned int iocPercent = 0;
    unsigned int fokPercent = 0;
    unsigned int marketPercent = 0;
    unsigned int sweepPercent = 0;
    unsigned int postOnlyPercent = 0;          // of passive submissions, and of the preload
    unsigned int unfillableFokPercent = 0;     // of FOKs, sized past what their limit reaches

    // Of replaces: same-price decreases (keep priority), price changes near the touch, same-price
    // increases (both lose priority), crossing replaces, and post-only replaces that would take.
    unsigned int decreasePercent = 35;
    unsigned int repricePercent = 40;
    unsigned int increasePercent = 10;
    unsigned int crossPercent = 10;
    unsigned int takerPercent = 5;

    // Owners on every order, preload included: ownerPercent have one from 1..ownerPool. Modes:
    // CANCEL_RESTING, then CANCEL_INCOMING, the rest CANCEL_BOTH.
    unsigned int ownerPercent = 0;
    OwnerId ownerPool = 1;
    // Of owned orders, topOwnerPercent belong to owners 1..topOwners, the rest to the whole
    // pool, so a few large participants meet themselves often enough to exercise prevention.
    unsigned int topOwnerPercent = 0;
    OwnerId topOwners = 1;
    unsigned int cancelRestingPercent = 100;
    unsigned int cancelIncomingPercent = 0;

    // Stops: preloadStops pending stops before timing; then stopLimitPercent of new stops are
    // STOP_LIMIT (restingLimitPercent of those with a limit that rests once triggered) and the
    // rest STOP + IOC. A timed stop's price lies 1 to stopReachTicks on the untriggered side
    // of the last trade price (the opposing best before any trade), so stops gather in
    // clusters near the price, and one cluster's trades can reach the next; a preloaded one
    // lies 1 to preloadStopSpread beyond the touch. A sweep goes up to sweepTicks through.
    std::size_t preloadStops = 0;
    Price sweepTicks = 3;
    Price preloadStopSpread = 100;
    unsigned int stopLimitPercent = 70;
    unsigned int restingLimitPercent = 50;
    Price stopReachTicks = 8;

    // Bounds that hold the book's shape at every size: below restingFloor resting orders,
    // submissions rest; above restingCap, they trade; likewise new stops and cancels of stops
    // turn into each other outside [pendingFloor, pendingCap]. 0 turns a bound off.
    std::size_t restingFloor = 0;
    std::size_t restingCap = 0;
    std::size_t pendingFloor = 0;
    std::size_t pendingCap = 0;
};

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

    WorkloadFamily family = WorkloadFamily::V1;
    BehaviorMix behavior;  // BEHAVIOR workloads only
};

// Throws std::invalid_argument, naming the workload and the problem, for a configuration that
// cannot be generated: a mix that does not sum to 100, fewer than 2 levels, fewer initial orders than
// levels, or a band that does not fit between price 1 and the largest Price.
void validateWorkloadConfig(const WorkloadConfig& config);

// The five built-in v1 workloads, in a fixed order: small-book, deep-book, matching-heavy,
// cancellation-heavy, mostly-resting.
const std::vector<WorkloadConfig>& builtInWorkloads();
// The behavior workloads, in a fixed order: quote-refresh, pending-stops, self-trade,
// immediate, matching-heavy-request, matching-heavy-owned.
const std::vector<WorkloadConfig>& behaviorWorkloads();
// Throws std::invalid_argument, listing the known names, for an unknown one. Searches both.
const WorkloadConfig& findWorkload(std::string_view name);

enum class CommandType { SUBMIT, CANCEL, REPLACE };

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

// One command of a behavior workload: a request to submit, a replace, or the ID of a
// resting order or pending stop to cancel (request.id alone).
struct BehaviorCommand {
    CommandType type = CommandType::SUBMIT;
    OrderRequest request{};
    ReplaceRequest replacement{};

    bool operator==(const BehaviorCommand&) const = default;
};

// What the timed commands do, as the model predicts it for the generator and as a runner
// counts it from the book's results. The two must agree on every run.
struct WorkloadOutcome {
    std::size_t submits = 0;
    std::size_t cancels = 0;      // of resting orders and of pending stops
    std::size_t rejections = 0;   // always 0 for a generated workload, refused replaces included
    std::size_t trades = 0;       // the cascades' included
    std::uint64_t volume = 0;     // the sum of every trade's quantity
    std::size_t finalActiveOrders = 0;
    std::size_t finalPriceLevels = 0;  // both sides
    std::size_t replaces = 0;
    std::size_t expirations = 0;  // OrderExpired events: accepted orders whose remainder expired
    std::size_t preventions = 0;  // SelfTradePrevented events
    std::size_t stopsTriggered = 0;
    std::size_t finalPendingStops = 0;

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
    // A BEHAVIOR workload's commands instead, likewise; the two above stay empty for it.
    std::vector<BehaviorCommand> behaviorPreload;
    std::vector<BehaviorCommand> behaviorTimed;
    WorkloadOutcome expected;
    // How many of the preload's orders rest, for reserving the book.
    std::size_t preloadResting = 0;

    std::size_t timedCount() const { return config.family == WorkloadFamily::V1 ? timed.size() : behaviorTimed.size(); }
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
// The same over the commands and v1's outcome counters alone: workloadFingerprint as it was
// under workloadGeneratorVersion 2, so a test can show the five v1 workloads' commands did
// not change.
std::uint64_t workloadFingerprintV2(const Workload& workload);

// generateWorkload for a BEHAVIOR configuration (behavior_workload.cpp).
Workload generateBehaviorWorkload(const WorkloadConfig& config, std::size_t timedCommands, std::uint64_t seed);

}  // namespace order_book::benchmark
