#include "benchmark_report.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iterator>
#include <system_error>

#include "order_book/invariants.hpp"

// The CMake configuration the program was built in, passed by CMakeLists.txt.
#ifndef ORDER_BOOK_BUILD_CONFIG
#define ORDER_BOOK_BUILD_CONFIG "unknown"
#endif

// GCC and MSVC define __SANITIZE_ADDRESS__ under AddressSanitizer; Clang reports it through
// __has_feature.
#if defined(__SANITIZE_ADDRESS__)
#define ORDER_BOOK_ADDRESS_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ORDER_BOOK_ADDRESS_SANITIZER 1
#endif
#endif
#ifndef ORDER_BOOK_ADDRESS_SANITIZER
#define ORDER_BOOK_ADDRESS_SANITIZER 0
#endif

// GCC defines no macro under UBSan, so CMakeLists.txt defines this when ORDER_BOOK_ENABLE_UBSAN
// applies it.
#ifndef ORDER_BOOK_UNDEFINED_SANITIZER
#define ORDER_BOOK_UNDEFINED_SANITIZER 0
#endif

namespace order_book::benchmark {

namespace {

std::string compilerName() {
#if defined(__clang__)
    return "Clang " __clang_version__;
#elif defined(__GNUC__)
    return "GCC " __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#else
    return "unknown";
#endif
}

const char* boolText(bool value) {
    return value ? "true" : "false";
}

std::string jsonString(const std::string& text) {
    std::string quoted = "\"";
    for (const char c : text) {
        switch (c) {
            case '"': quoted += "\\\""; break;
            case '\\': quoted += "\\\\"; break;
            case '\n': quoted += "\\n"; break;
            case '\r': quoted += "\\r"; break;
            case '\t': quoted += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    quoted += "\\u00";
                    quoted += hex[(c >> 4) & 0xF];
                    quoted += hex[c & 0xF];
                } else {
                    quoted += c;
                }
        }
    }
    return quoted + "\"";
}

std::string hexText(std::uint64_t value) {
    char buffer[16];
    const std::to_chars_result result = std::to_chars(std::begin(buffer), std::end(buffer), value, 16);
    return "0x" + std::string(buffer, result.ptr);
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t middle = values.size() / 2;
    return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}

MeasurementSummary summarizeGroup(const std::vector<const Measurement*>& group) {
    MeasurementSummary summary;
    summary.workload = group.front()->workload;
    summary.kind = group.front()->kind;
    summary.repetitions = group.size();
    summary.commands = group.front()->commands;

    std::vector<double> rates;
    for (const Measurement* measurement : group) {
        rates.push_back(measurement->operationsPerSecond());
    }
    summary.minimum = *std::min_element(rates.begin(), rates.end());
    summary.maximum = *std::max_element(rates.begin(), rates.end());
    double total = 0;
    for (const double rate : rates) {
        total += rate;
    }
    summary.mean = total / static_cast<double>(rates.size());
    summary.median = median(rates);
    if (rates.size() > 1) {
        double squares = 0;
        for (const double rate : rates) {
            squares += (rate - summary.mean) * (rate - summary.mean);
        }
        summary.standardDeviation = std::sqrt(squares / static_cast<double>(rates.size() - 1));
    }
    summary.coefficientOfVariation = summary.mean > 0 ? summary.standardDeviation / summary.mean : 0;
    return summary;
}

std::string padRight(std::string text, std::size_t width) {
    if (text.size() < width) {
        text.append(width - text.size(), ' ');
    }
    return text;
}

std::string padLeft(const std::string& text, std::size_t width) {
    return text.size() < width ? std::string(width - text.size(), ' ') + text : text;
}

std::string outcomeJson(const WorkloadOutcome& outcome) {
    return "{\"submits\": " + std::to_string(outcome.submits) + ", \"cancels\": " + std::to_string(outcome.cancels) +
           ", \"cancellation_rate\": " + formatFixed(outcome.cancellationRate(), 6) +
           ", \"rejections\": " + std::to_string(outcome.rejections) + ", \"trades\": " +
           std::to_string(outcome.trades) + ", \"volume\": " + std::to_string(outcome.volume) +
           ", \"final_active_orders\": " + std::to_string(outcome.finalActiveOrders) +
           ", \"final_price_levels\": " + std::to_string(outcome.finalPriceLevels) +
           ", \"replaces\": " + std::to_string(outcome.replaces) +
           ", \"expirations\": " + std::to_string(outcome.expirations) +
           ", \"preventions\": " + std::to_string(outcome.preventions) +
           ", \"stops_triggered\": " + std::to_string(outcome.stopsTriggered) +
           ", \"final_pending_stops\": " + std::to_string(outcome.finalPendingStops) + "}";
}

std::string behaviorJson(const WorkloadConfig& config) {
    if (config.family != WorkloadFamily::BEHAVIOR) {
        return "";
    }
    const BehaviorMix& mix = config.behavior;
    if (!mix.replaysWorkload.empty()) {
        return ", \"behavior\": {\"replays\": " + jsonString(mix.replaysWorkload) +
               ", \"owner_per_order\": " + boolText(mix.ownerPerOrder) + "}";
    }
    return ", \"behavior\": {\"commands_percent\": {\"submit\": " + std::to_string(mix.submitPercent) +
           ", \"replace\": " + std::to_string(mix.replacePercent) + ", \"cancel\": " + std::to_string(mix.cancelPercent) +
           ", \"stop\": " + std::to_string(mix.stopPercent) + ", \"cancel_stop\": " + std::to_string(mix.cancelStopPercent) +
           "}, \"submissions_percent\": {\"passive\": " + std::to_string(mix.passivePercent) +
           ", \"marketable\": " + std::to_string(mix.marketablePercent) + ", \"ioc\": " + std::to_string(mix.iocPercent) +
           ", \"fok\": " + std::to_string(mix.fokPercent) + ", \"market\": " + std::to_string(mix.marketPercent) +
           ", \"sweep\": " + std::to_string(mix.sweepPercent) + "}, \"post_only_percent\": " +
           std::to_string(mix.postOnlyPercent) + ", \"unfillable_fok_percent\": " + std::to_string(mix.unfillableFokPercent) +
           ", \"replaces_percent\": {\"decrease\": " + std::to_string(mix.decreasePercent) +
           ", \"reprice\": " + std::to_string(mix.repricePercent) + ", \"increase\": " + std::to_string(mix.increasePercent) +
           ", \"cross\": " + std::to_string(mix.crossPercent) + ", \"taker\": " + std::to_string(mix.takerPercent) +
           "}, \"owner_percent\": " + std::to_string(mix.ownerPercent) + ", \"owner_pool\": " + std::to_string(mix.ownerPool) +
           ", \"preload_stops\": " + std::to_string(mix.preloadStops) + ", \"stop_reach_ticks\": " +
           std::to_string(mix.stopReachTicks) + ", \"resting_bounds\": [" + std::to_string(mix.restingFloor) + ", " +
           std::to_string(mix.restingCap) + "], \"pending_bounds\": [" + std::to_string(mix.pendingFloor) + ", " +
           std::to_string(mix.pendingCap) + "]}";
}

std::string workloadJson(const WorkloadRecord& record) {
    const WorkloadConfig& config = record.config;
    return "{\"name\": " + jsonString(config.name) + ", \"objective\": " + jsonString(config.objective) +
           ", \"fingerprint\": \"" + hexText(record.fingerprint) + "\"" +
           ", \"initial_orders\": " + std::to_string(config.initialOrders) +
           ", \"target_levels\": " + std::to_string(config.targetLevels) +
           ", \"mid_price\": " + std::to_string(config.midPrice) +
           ", \"cancel_percent\": " + std::to_string(config.cancelPercent) +
           ", \"buy_percent\": " + std::to_string(config.buyPercent) +
           ", \"price_modes_percent\": {\"uniform\": " + std::to_string(config.uniformPercent) +
           ", \"concentrated\": " + std::to_string(config.concentratedPercent) +
           ", \"marketable\": " + std::to_string(config.marketablePercent) + "}" +
           ", \"concentrated_ticks\": " + std::to_string(config.concentratedTicks) +
           ", \"through_ticks\": " + std::to_string(config.throughTicks) +
           ", \"active_order_cap\": " + std::to_string(config.activeOrderCap) +
           ", \"quantity_classes_percent\": {\"small\": " + std::to_string(config.smallPercent) +
           ", \"medium\": " + std::to_string(config.mediumPercent) +
           ", \"large\": " + std::to_string(config.largePercent) + "}" +
           ", \"preload_orders\": " + std::to_string(record.preloadOrders) +
           ", \"timed_commands\": " + std::to_string(record.timedCommands) +
           ", \"reserved_orders\": " + std::to_string(record.reservedOrders) +
           ", \"generation_ns\": " + std::to_string(record.generationNanoseconds) + behaviorJson(config) +
           ", \"expected\": " + outcomeJson(record.expected) + "}";
}

std::string summaryJson(const MeasurementSummary& summary) {
    return "{\"workload\": " + jsonString(summary.workload) + ", \"kind\": \"" + benchmarkKindName(summary.kind) +
           "\", \"repetitions\": " + std::to_string(summary.repetitions) +
           ", \"commands\": " + std::to_string(summary.commands) +
           ", \"ops_per_second\": {\"min\": " + formatFixed(summary.minimum, 3) +
           ", \"median\": " + formatFixed(summary.median, 3) + ", \"mean\": " + formatFixed(summary.mean, 3) +
           ", \"max\": " + formatFixed(summary.maximum, 3) +
           ", \"stddev\": " + formatFixed(summary.standardDeviation, 3) + "}" +
           ", \"coefficient_of_variation\": " + formatFixed(summary.coefficientOfVariation, 6) + "}";
}

std::string measurementJson(const Measurement& measurement) {
    return "{\"workload\": " + jsonString(measurement.workload) + ", \"kind\": \"" +
           benchmarkKindName(measurement.kind) + "\", \"repetition\": " + std::to_string(measurement.repetition) +
           ", \"warmup\": " + boolText(measurement.warmup) + ", \"commands\": " +
           std::to_string(measurement.commands) + ", \"elapsed_ns\": " +
           std::to_string(measurement.elapsedNanoseconds) + ", \"ops_per_second\": " +
           formatFixed(measurement.operationsPerSecond(), 3) + ", \"ns_per_command\": " +
           formatFixed(measurement.nanosecondsPerCommand(), 3) + ", \"outcome\": " +
           outcomeJson(measurement.outcome) + "}";
}

// Each element on its own indented line, comma separated, or [] when there are none.
template <typename Item, typename Format>
std::string jsonArray(const std::vector<Item>& items, Format format) {
    if (items.empty()) {
        return "[]";
    }
    std::string array = "[\n";
    for (std::size_t i = 0; i < items.size(); ++i) {
        array += "    " + format(items[i]) + (i + 1 < items.size() ? ",\n" : "\n");
    }
    return array + "  ]";
}

}  // namespace

BuildInfo currentBuild() {
    BuildInfo build;
    build.config = ORDER_BOOK_BUILD_CONFIG;
    build.compiler = compilerName();
#ifdef NDEBUG
    build.ndebug = true;
#endif
    build.invariantChecks = invariantChecksAfterCommands;
    build.addressSanitizer = ORDER_BOOK_ADDRESS_SANITIZER != 0;
    build.undefinedBehaviorSanitizer = ORDER_BOOK_UNDEFINED_SANITIZER != 0;
    return build;
}

std::string formatFixed(double value, int decimals) {
    char buffer[64];
    const std::to_chars_result result =
        std::to_chars(std::begin(buffer), std::end(buffer), value, std::chars_format::fixed, decimals);
    if (result.ec != std::errc()) {
        return "0";
    }
    return std::string(buffer, result.ptr);
}

std::vector<MeasurementSummary> summarize(const std::vector<Measurement>& measurements) {
    std::vector<std::vector<const Measurement*>> groups;
    for (const Measurement& measurement : measurements) {
        if (measurement.warmup) {
            continue;
        }
        const auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& candidate) {
            return candidate.front()->workload == measurement.workload && candidate.front()->kind == measurement.kind;
        });
        if (group == groups.end()) {
            groups.push_back({&measurement});
        } else {
            group->push_back(&measurement);
        }
    }

    std::vector<MeasurementSummary> summaries;
    for (const auto& group : groups) {
        summaries.push_back(summarizeGroup(group));
    }
    return summaries;
}

std::string formatMeasurementsCsv(const BenchmarkReport& report) {
    std::string csv =
        "workload,kind,size,seed,generator_version,build_config,optimized,repetition,warmup,commands,"
        "elapsed_ns,ops_per_second,ns_per_command,submits,cancels,cancellation_rate,rejections,trades,"
        "volume,final_active_orders,final_price_levels,replaces,expirations,preventions,stops_triggered,"
        "final_pending_stops\n";
    const std::string session = report.size + "," + std::to_string(report.seed) + "," +
                                std::to_string(workloadGeneratorVersion) + "," + report.build.config + "," +
                                boolText(report.build.optimized());
    for (const Measurement& measurement : report.measurements) {
        const WorkloadOutcome& outcome = measurement.outcome;
        csv += measurement.workload + "," + benchmarkKindName(measurement.kind) + "," + session + "," +
               std::to_string(measurement.repetition) + "," + boolText(measurement.warmup) + "," +
               std::to_string(measurement.commands) + "," + std::to_string(measurement.elapsedNanoseconds) + "," +
               formatFixed(measurement.operationsPerSecond(), 3) + "," +
               formatFixed(measurement.nanosecondsPerCommand(), 3) + "," + std::to_string(outcome.submits) + "," +
               std::to_string(outcome.cancels) + "," + formatFixed(outcome.cancellationRate(), 6) + "," +
               std::to_string(outcome.rejections) + "," + std::to_string(outcome.trades) + "," +
               std::to_string(outcome.volume) + "," + std::to_string(outcome.finalActiveOrders) + "," +
               std::to_string(outcome.finalPriceLevels) + "," + std::to_string(outcome.replaces) + "," +
               std::to_string(outcome.expirations) + "," + std::to_string(outcome.preventions) + "," +
               std::to_string(outcome.stopsTriggered) + "," + std::to_string(outcome.finalPendingStops) + "\n";
    }
    return csv;
}

std::string formatReportJson(const BenchmarkReport& report) {
    const BuildInfo& build = report.build;
    return "{\n"
           "  \"generator_version\": " + std::to_string(workloadGeneratorVersion) + ",\n" +
           "  \"seed\": " + std::to_string(report.seed) + ",\n" +
           "  \"size\": " + jsonString(report.size) + ",\n" +
           "  \"timed_commands\": " + std::to_string(report.timedCommands) + ",\n" +
           "  \"warmups\": " + std::to_string(report.settings.warmups) + ",\n" +
           "  \"repetitions\": " + std::to_string(report.settings.repetitions) + ",\n" +
           "  \"started_at_unix_seconds\": " + std::to_string(report.startedAtUnixSeconds) + ",\n" +
           "  \"build\": {\"config\": " + jsonString(build.config) + ", \"compiler\": " + jsonString(build.compiler) +
           ", \"optimized\": " + boolText(build.optimized()) + ", \"ndebug\": " + boolText(build.ndebug) +
           ", \"invariant_checks\": " + boolText(build.invariantChecks) +
           ", \"address_sanitizer\": " + boolText(build.addressSanitizer) +
           ", \"undefined_behavior_sanitizer\": " + boolText(build.undefinedBehaviorSanitizer) + "},\n" +
           "  \"workloads\": " + jsonArray(report.workloads, workloadJson) + ",\n" +
           "  \"summaries\": " + jsonArray(summarize(report.measurements), summaryJson) + ",\n" +
           "  \"measurements\": " + jsonArray(report.measurements, measurementJson) + "\n" +
           "}\n";
}

std::string formatSummaryTable(const std::vector<MeasurementSummary>& summaries) {
    const std::size_t nameWidth = 20;
    std::string table = padRight("workload", nameWidth) + padRight("kind", 8) + padLeft("reps", 5) +
                        padLeft("commands", 10) + padLeft("median ops/s", 15) + padLeft("min ops/s", 15) +
                        padLeft("max ops/s", 15) + padLeft("ns/cmd", 9) + padLeft("cv", 8) + "\n";
    for (const MeasurementSummary& summary : summaries) {
        const double nanoseconds = summary.median > 0 ? 1e9 / summary.median : 0;
        table += padRight(summary.workload, nameWidth) + padRight(benchmarkKindName(summary.kind), 8) +
                 padLeft(std::to_string(summary.repetitions), 5) + padLeft(std::to_string(summary.commands), 10) +
                 padLeft(formatFixed(summary.median, 0), 15) + padLeft(formatFixed(summary.minimum, 0), 15) +
                 padLeft(formatFixed(summary.maximum, 0), 15) + padLeft(formatFixed(nanoseconds, 1), 9) +
                 padLeft(formatFixed(summary.coefficientOfVariation * 100, 1) + "%", 8) + "\n";
    }
    return table;
}

}  // namespace order_book::benchmark
