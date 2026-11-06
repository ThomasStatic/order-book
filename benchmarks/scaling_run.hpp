#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "benchmark_log.hpp"
#include "benchmark_runner.hpp"
#include "interleaver.hpp"
#include "order_book/network/gateway_thread.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/matching_loop.hpp"
#include "workload.hpp"

// The threaded kinds at N shards and M producers or gateway threads, #33's scaling ladder: each
// shard's books on a matching thread of their own, every thread placed by a ThreadPlan. Not public
// API. See docs/internals/benchmarks.md.
namespace order_book::benchmark {

// Where a run's instruments go. EVEN puts instrument i on shard i modulo N, so every shard holds as
// many; HASH leaves each to its symbol's hash, as an engine places an unpinned instrument; and
// HOT_ALONE puts instrument 0, a skewed run's top one, alone on shard 0, and the rest evenly on the
// others: the remedy OPERATIONS documents for a hot instrument.
enum class Placement { EVEN, HASH, HOT_ALONE };

const char* placementName(Placement placement);
// Throws std::invalid_argument on a name other than even, hash, or hot-alone.
Placement parsePlacement(std::string_view name);

const char* spreadName(reference::Spread spread);
// Throws std::invalid_argument on a name other than uniform or skewed: one-hot is a test spread.
reference::Spread parseSpread(std::string_view name);

// Each instrument's share of total commands: equal under UNIFORM, and under SKEWED by its Zipf
// weight 1/(k+1), so instrument 0 takes the most. Every share is at least one, and they sum to
// total. Throws std::invalid_argument if total is below instruments.
std::vector<std::size_t> commandShares(std::size_t total, std::size_t instruments, reference::Spread spread);

// instrumentWorkloads, but with each instrument's commands as commandShares gives them. Under
// UNIFORM with total a multiple of instruments it is exactly instrumentWorkloads.
std::vector<Workload> spreadWorkloads(const WorkloadConfig& config, std::size_t total, std::uint64_t seed, std::size_t instruments,
                                      reference::Spread spread);

// #33's mixed recipe: two each of deep-book, pending-stops, immediate, and quote-refresh in every
// eight, each its own stream as instrumentWorkloads makes them, with total shared out evenly.
// Instrument i takes the recipe's (i / shards) modulo 8, so under even placement every shard's
// eight hold two of each.
std::vector<Workload> mixedWorkloads(std::size_t total, std::uint64_t seed, std::size_t instruments, std::size_t shards = 1);

// A threaded run's commands as one number: each instrument's workloadFingerprint, in order, with
// what fixes the order each shard takes them in: the spread its draws go by, the shard count, and
// the placement, with the seeds in the instruments' own. interleaver_tests pins the interleaver's
// own sequences.
std::uint64_t runFingerprint(const std::vector<Workload>& workloads, reference::Spread spread, std::size_t shards, Placement placement);

// Every workload placed on one of shards, each reserved to its peak, and each shard's instruments.
struct RunPlacement {
    std::shared_ptr<const runtime::InstrumentDirectory> directory;
    std::vector<std::vector<std::uint32_t>> instrumentsOf;  // by shard, in instrument order
};

// Throws std::invalid_argument if a shard would hold no book, or HOT_ALONE has one shard.
RunPlacement placeInstruments(const std::vector<Workload>& workloads, std::size_t shards, Placement placement);

// The logical processor for each of a run's threads, in this order: its N matching threads (a
// shard run's N threads), then its M producers or gateway threads, then a gateway run's M client
// threads, then, if listed, its N log writers, each of whose housekeeping thread and roll-forward
// worker goes where it does. Empty pins nothing. Every thread placed is raised to high priority
// if asked, whether pinned or not.
struct ThreadPlan {
    std::vector<unsigned int> cores;
    bool highPriority = false;
    std::string layout = "unpinned";  // the name the results carry
};

struct ScalingSettings {
    std::size_t shards = 1;   // N
    std::size_t threads = 1;  // M: QUEUE's producers, GATEWAY's gateway threads and client threads
    reference::Spread spread = reference::Spread::UNIFORM;
    Placement placement = Placement::EVEN;
    ThreadPlan plan;
    RunDurability durability = RunDurability::NONE;
    LogFileFill fill = LogFileFill::EXTENDED;
    std::filesystem::path logParent = defaultLogParent();
    std::size_t window = defaultGatewayWindow;            // GATEWAY's, per connection
    std::size_t framesPerPass = network::framesPerPass;   // GATEWAY's frame budget; the most a size_t holds is unlimited
    std::chrono::nanoseconds spinWindow = runtime::defaultSpinWindow;
    // With a log, a roll-forward worker per shard, as the engine runs them, on that shard's log
    // writer's core with its housekeeping thread.
    bool rollWorker = false;
    std::uint64_t logFileBytes = runtime::defaultLogFileBytes;  // a test's smaller, so files close while it runs
};

// SHARD, QUEUE, or GATEWAY at settings' N and M, timed from one start until the last part finishes,
// with each shard's rate, to its last command's answer, in the measurement's partRates. QUEUE's shard i belongs to producer i modulo M, and GATEWAY's
// instrument k to client thread k modulo M, a connection each, so every instrument stays on one
// producer or connection and every book's prediction holds; each is checked, and a book that grew
// fails the run. Throws std::invalid_argument for another kind or a plan that names too few cores.
Measurement runScaledOnce(const std::vector<Workload>& workloads, BenchmarkKind kind, const ScalingSettings& settings);

// settings.warmups runs and then settings.repetitions measured runs of runScaledOnce.
std::vector<Measurement> runScaledRepeated(const std::vector<Workload>& workloads, BenchmarkKind kind, const ScalingSettings& scaling,
                                           const RunSettings& settings);

}  // namespace order_book::benchmark
