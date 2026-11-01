#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "histogram.hpp"
#include "noise_floor.hpp"

// A latency result in the only form the claims rule in docs/LATENCY.md lets anyone quote: one
// interval's percentiles, merged over every run, beside each run's median and range, with the
// run record and noise floors. Formatting is locale-free.
namespace order_book::latency {

inline constexpr std::size_t minimumRuns = 9;

// A value within this many clock steps of the empty pair is unresolved.
inline constexpr double unresolvedSteps = 3.0;

// Gaps count as "about as frequent" as a percentile's tail within this factor.
inline constexpr double noiseMarkFactor = 4.0;

// Every field a quotable run must carry, in a fixed order. A field starts unfilled, there is
// no default for one, and an unfilled field makes the run unquotable. Where a field does not
// apply to an interval, the program says so in the value.
class RunRecord {
public:
    RunRecord();

    // Throws std::invalid_argument for a name that is not a field.
    void set(std::string_view name, std::string value);
    // The unfilled fields, in record order.
    std::vector<std::string> missing() const;
    // One indented "name=value\n" line per field, with MISSING for an unfilled one.
    std::string format() const;

private:
    struct Field {
        std::string name;
        std::optional<std::string> value;
    };
    std::vector<Field> fields_;
};

// One row of a report: one interval, at one rate, on one workload, over all of its runs.
struct ReportRow {
    std::string title;
    RunRecord record;
    std::vector<Histogram> runs;  // every measured run: none is ever dropped
    double ticksPerNanosecond = 1.0;
    double stepNanoseconds = 0;
    double emptyPairP50Nanoseconds = 0;
    std::string clockName;
    std::optional<NoiseFloor> noiseBefore;
    std::optional<NoiseFloor> noiseAfter;
    // How often the tail of a percentile happens, to compare with the noise floor's gaps.
    double commandsPerSecond = 0;
    bool optimizedBuild = false;
    bool openLoop = false;
    bool pinnedAtHighPriority = false;
    bool sustained = true;  // every run's backlog
};

// Why a row can't be quoted, in a fixed order; empty if it can.
std::vector<std::string> unquotableReasons(const ReportRow& row);

// The row for people, with its record, noise floors, and percentiles. A percentile with fewer
// than 1,000 samples beyond it prints as such, in the merged column and in the runs' columns
// separately. A row that was not sustained prints no percentile at all.
std::string formatRow(const ReportRow& row);

}  // namespace order_book::latency
