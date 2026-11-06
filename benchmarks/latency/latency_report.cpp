#include "latency_report.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

#include "benchmark_report.hpp"
#include "clock.hpp"

namespace order_book::latency {

namespace {

using benchmark::formatFixed;

constexpr std::array<std::string_view, 31> recordFieldNames{
    // The interval.
    "interval", "time", "load", "target-rate", "achieved-rate",
    // The workload.
    "workload", "workload-fingerprint", "generator-version", "seed",
    // The threads.
    "shards", "gateway-threads", "active-connections", "idle-connections", "socket-set", "spin-window",
    "matching-cpu-share", "core-map", "pinned", "priority",
    // Durability.
    "durability", "log-files", "logs-share-drive",
    // The books.
    "book-capacity", "capacity-growths",
    // The measurement.
    "clock", "clock-step-ns", "clock-read-ns", "empty-pair-ns", "commands-per-run",
    // The environment.
    "build", "machine"};

std::string pad(std::string text, std::size_t width) {
    if (text.size() < width) {
        text.insert(0, width - text.size(), ' ');
    }
    return text;
}

std::string padRight(std::string text, std::size_t width) {
    if (text.size() < width) {
        text.append(width - text.size(), ' ');
    }
    return text;
}

constexpr std::size_t statWidth = 6;
constexpr std::size_t columnWidth = 12;

std::string nanoseconds(std::uint64_t ticks, const ReportRow& row) {
    return formatFixed(static_cast<double>(ticks) / row.ticksPerNanosecond, 0);
}

template <typename Value>
Value medianOf(std::vector<Value> values) {
    std::sort(values.begin(), values.end());
    return values[(values.size() - 1) / 2];  // the lower of the middle two, for an even count
}

std::string line(std::string_view stat, const std::string& merged, const std::string& beyond,
                 const std::string& median, const std::string& low, const std::string& high,
                 const std::vector<std::string>& marks) {
    std::string text = padRight(std::string(stat), statWidth) + pad(merged, columnWidth) + pad(beyond, columnWidth) +
                       pad(median, columnWidth) + pad(low, columnWidth) + pad(high, columnWidth);
    for (const std::string& mark : marks) {
        text += "  " + mark;
    }
    return text + '\n';
}

// A gap over T can put a sample past any value of T or more, so the gaps that could reach a
// percentile are those over the largest threshold not above it.
bool explainedByNoise(double valueNanoseconds, Percentile percentile, const ReportRow& row) {
    std::size_t threshold = 0;
    for (std::size_t i = 0; i < noiseThresholdsMicroseconds.size(); ++i) {
        if (noiseThresholdsMicroseconds[i] * 1000.0 <= valueNanoseconds) {
            threshold = i;
        }
    }
    double gaps = 0.0;
    for (const std::optional<NoiseFloor>& floor : {row.noiseBefore, row.noiseAfter}) {
        if (floor) {
            gaps = std::max(gaps, floor->gapsPerSecond(threshold));
        }
    }
    const double tailShare = static_cast<double>(100000 - percentile.partsPerHundredThousand) / 100000.0;
    return gaps > 0.0 && gaps * noiseMarkFactor >= row.commandsPerSecond * tailShare;
}

std::vector<std::string> marksFor(double valueNanoseconds, Percentile percentile, const ReportRow& row) {
    std::vector<std::string> marks;
    if (valueNanoseconds <= row.emptyPairP50Nanoseconds + unresolvedSteps * row.stepNanoseconds) {
        marks.emplace_back("unresolved");
    }
    if (explainedByNoise(valueNanoseconds, percentile, row)) {
        marks.emplace_back("noise");
    }
    return marks;
}

std::string percentileLine(Percentile percentile, const Histogram& merged, const ReportRow& row) {
    const std::uint64_t beyond = samplesBeyond(merged.count(), percentile);
    if (!printable(merged.count(), percentile)) {
        return padRight(std::string(percentile.name), statWidth) + "not printed: " + std::to_string(beyond) +
               " beyond, " + std::to_string(minimumSamplesBeyond) + " needed\n";
    }

    const std::uint64_t value = merged.valueAt(percentile);
    const double valueNanoseconds = static_cast<double>(value) / row.ticksPerNanosecond;
    std::string median = "-";
    std::string low = "-";
    std::string high = "-";
    std::vector<std::uint64_t> perRun;
    bool everyRunPrints = true;
    for (const Histogram& run : row.runs) {
        everyRunPrints = everyRunPrints && printable(run.count(), percentile);
        perRun.push_back(run.valueAt(percentile));
    }
    if (everyRunPrints) {
        median = nanoseconds(medianOf(perRun), row);
        low = nanoseconds(*std::min_element(perRun.begin(), perRun.end()), row);
        high = nanoseconds(*std::max_element(perRun.begin(), perRun.end()), row);
    }
    return line(percentile.name, nanoseconds(value, row), std::to_string(beyond), median, low, high,
                marksFor(valueNanoseconds, percentile, row));
}

std::string maximumLine(const ReportRow& row) {
    std::vector<std::uint64_t> maxima;
    for (const Histogram& run : row.runs) {
        maxima.push_back(run.maximum());
    }
    const std::uint64_t worst = *std::max_element(maxima.begin(), maxima.end());
    return line("max", nanoseconds(worst, row), "-", nanoseconds(medianOf(maxima), row),
                nanoseconds(*std::min_element(maxima.begin(), maxima.end()), row), nanoseconds(worst, row), {});
}

std::string noiseLine(std::string_view name, const std::optional<NoiseFloor>& floor) {
    std::string text = "noise floor " + std::string(name) + ": ";
    if (!floor) {
        return text + "not measured\n";
    }
    text += formatFixed(floor->seconds, 1) + " s; gaps/s";
    for (std::size_t i = 0; i < noiseThresholdsMicroseconds.size(); ++i) {
        text += " over " + formatFixed(noiseThresholdsMicroseconds[i], 0) + " us " +
                formatFixed(floor->gapsPerSecond(i), 1);
        text += i + 1 < noiseThresholdsMicroseconds.size() ? "," : ";";
    }
    return text + " max " + formatFixed(floor->maximumNanoseconds, 0) + " ns; lost " +
           formatFixed(floor->lostShare * 100.0, 2) + "%\n";
}

}  // namespace

RunRecord::RunRecord() {
    for (const std::string_view name : recordFieldNames) {
        fields_.push_back({std::string(name), std::nullopt});
    }
}

void RunRecord::set(std::string_view name, std::string value) {
    for (Field& field : fields_) {
        if (field.name == name) {
            field.value = std::move(value);
            return;
        }
    }
    throw std::invalid_argument("no run record field named '" + std::string(name) + "'");
}

std::vector<std::string> RunRecord::missing() const {
    std::vector<std::string> names;
    for (const Field& field : fields_) {
        if (!field.value) {
            names.push_back(field.name);
        }
    }
    return names;
}

std::string RunRecord::format() const {
    std::string text;
    for (const Field& field : fields_) {
        text += "  " + field.name + '=' + field.value.value_or("MISSING") + '\n';
    }
    return text;
}

std::vector<std::string> unquotableReasons(const ReportRow& row) {
    std::vector<std::string> reasons;
    for (const std::string& field : row.record.missing()) {
        reasons.push_back("missing " + field);
    }
    if (row.runs.size() < minimumRuns) {
        reasons.push_back(std::to_string(row.runs.size()) + " runs, at least " + std::to_string(minimumRuns) + " needed");
    }
    if (!row.optimizedBuild) {
        reasons.emplace_back("unoptimized build");
    }
    if (row.clockName != tscClockName) {
        reasons.push_back("clock is " + row.clockName);
    }
    if (!row.noiseBefore || !row.noiseAfter) {
        reasons.emplace_back("no noise floor on both sides");
    }
    if (row.openLoop && !row.pinnedAtHighPriority) {
        reasons.emplace_back("unpinned");
    }
    if (!row.sustained) {
        reasons.emplace_back("not sustained");
    }
    // A run whose commands all fell before its discard ended, say, has nothing to quote.
    const auto emptyRuns = std::count_if(row.runs.begin(), row.runs.end(), [](const Histogram& run) { return run.count() == 0; });
    if (emptyRuns > 0) {
        reasons.push_back(std::to_string(emptyRuns) + " of " + std::to_string(row.runs.size()) + " runs recorded nothing");
    }
    return reasons;
}

std::string formatRow(const ReportRow& row) {
    std::string text = "== " + row.title + " ==\n";
    const std::vector<std::string> reasons = unquotableReasons(row);
    if (reasons.empty()) {
        text += "quotable: yes\n";
    } else {
        text += "quotable: no (";
        for (std::size_t i = 0; i < reasons.size(); ++i) {
            text += (i == 0 ? "" : "; ") + reasons[i];
        }
        text += ")\n";
    }

    Histogram merged;
    for (const Histogram& run : row.runs) {
        merged.merge(run);
    }
    text += "runs: " + std::to_string(row.runs.size()) + ", merged samples: " + std::to_string(merged.count()) + '\n';
    text += "run record:\n" + row.record.format();
    text += noiseLine("before", row.noiseBefore);
    text += noiseLine("after", row.noiseAfter);
    if (row.openLoop && !row.pinnedAtHighPriority) {
        text += "unpinned\n";
    }
    if (!row.sustained) {
        return text + "not sustained\n";
    }

    text += padRight("stat", statWidth) + pad("merged ns", columnWidth) + pad("beyond", columnWidth) +
            pad("run median", columnWidth) + pad("run min", columnWidth) + pad("run max", columnWidth) + '\n';
    for (const Percentile percentile : reportedPercentiles) {
        text += percentileLine(percentile, merged, row);
    }
    return text + maximumLine(row);
}

}  // namespace order_book::latency
