#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "benchmark_runner.hpp"
#include "workload.hpp"

// Everything a benchmark session records, and its CSV, JSON, and text forms. Formatting is
// locale-free (std::to_chars and std::to_string), so a report reads the same everywhere.
namespace order_book::benchmark {

// How this program was built, as far as it changes what a measurement means.
struct BuildInfo {
    std::string config;       // the CMake configuration, e.g. Release
    std::string compiler;
    bool ndebug = false;
    bool invariantChecks = false;  // invariantChecksAfterCommands: a full book check per command
    bool addressSanitizer = false;
    bool undefinedBehaviorSanitizer = false;

    // NDEBUG, no automatic invariant checks, and no sanitizer: the only builds whose numbers
    // say anything about the engine's speed.
    bool optimized() const {
        return ndebug && !invariantChecks && !addressSanitizer && !undefinedBehaviorSanitizer;
    }
};

BuildInfo currentBuild();

// A workload as generated for the session: its configuration, a fingerprint of its commands,
// what it should do, and how long generating it took (never part of any measurement).
struct WorkloadRecord {
    WorkloadConfig config;
    std::uint64_t fingerprint = 0;
    std::size_t preloadOrders = 0;
    std::size_t timedCommands = 0;
    WorkloadOutcome expected;
    std::int64_t generationNanoseconds = 0;
};

// The measured (non-warm-up) repetitions of one workload and kind. Rates are operations per
// second; the deviation is the sample standard deviation (0 for a single repetition), and
// the coefficient of variation is that over the mean.
struct MeasurementSummary {
    std::string workload;
    BenchmarkKind kind = BenchmarkKind::CORE;
    std::size_t repetitions = 0;
    std::size_t commands = 0;
    double minimum = 0;
    double median = 0;
    double mean = 0;
    double maximum = 0;
    double standardDeviation = 0;
    double coefficientOfVariation = 0;
};

struct BenchmarkReport {
    BuildInfo build;
    std::uint64_t seed = 0;
    std::string size;  // smoke, standard, extended, or custom for an explicit command count
    std::size_t timedCommands = 0;
    RunSettings settings;
    std::int64_t startedAtUnixSeconds = 0;
    std::vector<WorkloadRecord> workloads;
    // Every run, warm-ups included, in the order they ran.
    std::vector<Measurement> measurements;
};

// One summary per workload and kind, in the order they first appear; warm-ups are left out,
// and a group with only warm-ups gives none.
std::vector<MeasurementSummary> summarize(const std::vector<Measurement>& measurements);

// One header line, then one line per measurement, warm-ups included, each ending with '\n'.
std::string formatMeasurementsCsv(const BenchmarkReport& report);

// The whole report: session settings, build, workloads, summaries, and every measurement.
std::string formatReportJson(const BenchmarkReport& report);

// A table of summaries for people, one row per workload and kind.
std::string formatSummaryTable(const std::vector<MeasurementSummary>& summaries);

// value with exactly `decimals` digits after the point, independent of locale.
std::string formatFixed(double value, int decimals);

}  // namespace order_book::benchmark
