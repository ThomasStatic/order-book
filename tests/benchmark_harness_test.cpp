#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "benchmark_report.hpp"
#include "benchmark_runner.hpp"
#include "order_book/invariants.hpp"
#include "order_book/replay_engine.hpp"
#include "order_book/replay_parser.hpp"
#include "workload.hpp"

namespace {

using namespace order_book::benchmark;
using order_book::OrderId;
using order_book::Price;
using order_book::Side;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

template <typename Callable>
void expectThrows(Callable&& callable, const std::string& message) {
    try {
        callable();
    } catch (const std::exception&) {
        return;
    }

    std::cerr << "FAILED: " << message << std::endl;
    std::exit(EXIT_FAILURE);
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::string hex(std::uint64_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << value;
    return text.str();
}

// A copy of a built-in workload small enough to run against the book in a Debug build, where
// every command checks the whole book: at most 200 levels and a fiftieth of the preload (and
// of a behavior workload's preloaded stops and bounds).
WorkloadConfig scaledDown(const WorkloadConfig& config) {
    WorkloadConfig scaled = config;
    scaled.targetLevels = std::min(config.targetLevels, 200U);
    scaled.initialOrders = std::max<std::size_t>(scaled.targetLevels, config.initialOrders / 50);
    BehaviorMix& mix = scaled.behavior;
    mix.preloadStops /= 50;
    mix.restingFloor /= 50;
    mix.restingCap /= 50;
    mix.pendingFloor /= 50;
    mix.pendingCap /= 50;
    return scaled;
}

Measurement fabricated(const std::string& workload, BenchmarkKind kind, bool warmup, std::size_t repetition,
                       std::int64_t elapsedNanoseconds) {
    Measurement measurement;
    measurement.workload = workload;
    measurement.kind = kind;
    measurement.warmup = warmup;
    measurement.repetition = repetition;
    measurement.commands = 600;
    measurement.elapsedNanoseconds = elapsedNanoseconds;
    measurement.outcome.submits = 400;
    measurement.outcome.cancels = 200;
    measurement.outcome.trades = 50;
    measurement.outcome.volume = 700;
    measurement.outcome.finalActiveOrders = 90;
    measurement.outcome.finalPriceLevels = 12;
    return measurement;
}

void testSizes() {
    expect(timedCommandCount(BenchmarkSize::SMOKE) == 10000, "smoke runs 10k timed commands");
    expect(timedCommandCount(BenchmarkSize::STANDARD) == 100000, "standard runs 100k timed commands");
    expect(timedCommandCount(BenchmarkSize::EXTENDED) == 1000000, "extended runs 1M timed commands");
    for (const BenchmarkSize size : {BenchmarkSize::SMOKE, BenchmarkSize::STANDARD, BenchmarkSize::EXTENDED}) {
        expect(parseBenchmarkSize(benchmarkSizeName(size)) == size, "a size's name parses back to it");
    }
    expectThrows([] { parseBenchmarkSize("huge"); }, "an unknown size is refused");
    expectThrows([] { parseBenchmarkSize("Smoke"); }, "size names are case-sensitive");

    expect(parseBenchmarkKind("core") == BenchmarkKind::CORE, "core parses");
    expect(parseBenchmarkKind("replay") == BenchmarkKind::REPLAY, "replay parses");
    expectThrows([] { parseBenchmarkKind("both"); }, "both is the CLI's, not a kind");
}

void testQuantityRanges() {
    expect(quantityRange(QuantityClass::SMALL).low == 1 && quantityRange(QuantityClass::SMALL).high == 10,
           "small quantities are 1-10");
    expect(quantityRange(QuantityClass::MEDIUM).low == 11 && quantityRange(QuantityClass::MEDIUM).high == 100,
           "medium quantities are 11-100");
    expect(quantityRange(QuantityClass::LARGE).low == 101 && quantityRange(QuantityClass::LARGE).high == 1000,
           "large quantities are 101-1000");
}

void testBuiltInWorkloads() {
    const std::vector<std::string> names{"small-book", "deep-book", "matching-heavy", "cancellation-heavy",
                                         "mostly-resting"};
    expect(builtInWorkloads().size() == names.size(), "five built-in workloads");
    for (std::size_t i = 0; i < names.size(); ++i) {
        expect(builtInWorkloads()[i].name == names[i], "built-in workload " + names[i] + " in its place");
        expect(&findWorkload(names[i]) == &builtInWorkloads()[i], "findWorkload finds " + names[i]);
        validateWorkloadConfig(builtInWorkloads()[i]);
    }
    expectThrows([] { findWorkload("tiny-book"); }, "an unknown workload is refused");

    // The table the workloads were designed from: initial orders and target levels.
    const std::vector<std::pair<std::size_t, unsigned int>> shapes{
        {100, 10}, {100000, 2000}, {10000, 200}, {50000, 500}, {1000, 100}};
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        expect(builtInWorkloads()[i].initialOrders == shapes[i].first, names[i] + " initial orders");
        expect(builtInWorkloads()[i].targetLevels == shapes[i].second, names[i] + " target levels");
    }
}

void testConfigValidation() {
    const WorkloadConfig base = findWorkload("small-book");

    WorkloadConfig prices = base;
    prices.marketablePercent += 1;
    expectThrows([&] { validateWorkloadConfig(prices); }, "price modes must sum to 100");

    WorkloadConfig quantities = base;
    quantities.smallPercent = 90;
    expectThrows([&] { validateWorkloadConfig(quantities); }, "quantity classes must sum to 100");

    WorkloadConfig oneLevel = base;
    oneLevel.targetLevels = 1;
    expectThrows([&] { validateWorkloadConfig(oneLevel); }, "both sides need a level");

    WorkloadConfig thin = base;
    thin.initialOrders = thin.targetLevels - 1;
    expectThrows([&] { validateWorkloadConfig(thin); }, "every level needs an initial order");

    WorkloadConfig cancels = base;
    cancels.cancelPercent = 101;
    expectThrows([&] { validateWorkloadConfig(cancels); }, "a rate above 100% is refused");

    WorkloadConfig lowBand = base;
    lowBand.midPrice = lowBand.targetLevels / 2;
    expectThrows([&] { validateWorkloadConfig(lowBand); }, "the bid band must stay above price 0");

    WorkloadConfig highBand = base;
    highBand.midPrice = std::numeric_limits<Price>::max() - 2;
    expectThrows([&] { validateWorkloadConfig(highBand); }, "the ask band must fit in a Price");

    expectThrows([&] { generateWorkload(prices, 10, 1); }, "generation validates its configuration");
}

void testDeterminism() {
    for (const WorkloadConfig& config : builtInWorkloads()) {
        const Workload first = generateWorkload(config, 2000, 7);
        const Workload second = generateWorkload(config, 2000, 7);
        expect(first.preload == second.preload, config.name + ": the same seed gives the same preload");
        expect(first.timed == second.timed, config.name + ": the same seed gives the same timed commands");
        expect(first.expected == second.expected, config.name + ": the same seed predicts the same outcome");
        expect(workloadFingerprint(first) == workloadFingerprint(second), config.name + ": same fingerprint");

        const Workload other = generateWorkload(config, 2000, 8);
        expect(first.timed != other.timed, config.name + ": another seed gives other commands");
        expect(workloadFingerprint(first) != workloadFingerprint(other), config.name + ": another fingerprint");

        const Workload longer = generateWorkload(config, 3000, 7);
        expect(std::equal(first.timed.begin(), first.timed.end(), longer.timed.begin()),
               config.name + ": a longer run starts with the shorter run's commands");
    }
}

// Pins what each workload generates at the smoke size, so the stream is the same on every
// compiler and standard library, and a change to what is drawn cannot go unnoticed: it must
// bump workloadGeneratorVersion and re-pin these.
void testPinnedFingerprints() {
    expect(workloadGeneratorVersion == 3, "re-pin these fingerprints whenever workloadGeneratorVersion changes");
    const std::vector<std::pair<std::string, std::uint64_t>> pinned{
        {"small-book", 0x9b1e4b372b451be9ULL},
        {"deep-book", 0x85872a86cb0fcf6eULL},
        {"matching-heavy", 0x788622a292126437ULL},
        {"cancellation-heavy", 0x94756f6173fa6c39ULL},
        {"mostly-resting", 0x538c21fd4569cc73ULL},
        {"quote-refresh", 0xe487fb15dfad9064ULL},
        {"pending-stops", 0xc7faedd7210867ddULL},
        {"self-trade", 0xc59ccf802f6a027eULL},
        {"immediate", 0x5cf10f331c391c2aULL},
        {"matching-heavy-request", 0x3b561cc1febbdffbULL},
        {"matching-heavy-owned", 0x908a46d3caf7694aULL},
    };
    for (const auto& [name, fingerprint] : pinned) {
        const Workload workload = generateWorkload(findWorkload(name), timedCommandCount(BenchmarkSize::SMOKE), 42);
        const std::uint64_t actual = workloadFingerprint(workload);
        expect(actual == fingerprint, name + " fingerprint is " + hex(actual) + ", pinned " + hex(fingerprint));
    }
}

// The five v1 workloads' commands did not change with version 3: their v2 fingerprints, over
// the commands and v1's counters alone, are still what version 2 pinned.
void testV1WorkloadsUnchanged() {
    const std::vector<std::pair<std::string, std::uint64_t>> version2{
        {"small-book", 0xcf205caa3ffc8449ULL},
        {"deep-book", 0xf66145ef12d4f6aeULL},
        {"matching-heavy", 0x081c56a4ef2427d7ULL},
        {"cancellation-heavy", 0x2d31b2d4322ec299ULL},
        {"mostly-resting", 0xfee0e8b12f2dc293ULL},
    };
    for (const auto& [name, fingerprint] : version2) {
        const Workload workload = generateWorkload(findWorkload(name), timedCommandCount(BenchmarkSize::SMOKE), 42);
        const std::uint64_t actual = workloadFingerprintV2(workload);
        expect(actual == fingerprint, name + " commands changed: " + hex(actual) + ", version 2 " + hex(fingerprint));
        expect(workload.behaviorPreload.empty() && workload.behaviorTimed.empty(), name + " stays on v1's commands");
    }
}

void testBehaviorWorkloads() {
    const std::vector<std::string> names{"quote-refresh", "pending-stops", "self-trade", "immediate",
                                         "matching-heavy-request", "matching-heavy-owned"};
    expect(behaviorWorkloads().size() == names.size(), "six behavior workloads");
    for (std::size_t i = 0; i < names.size(); ++i) {
        const WorkloadConfig& config = behaviorWorkloads()[i];
        expect(config.name == names[i], "behavior workload " + names[i] + " in its place");
        expect(&findWorkload(names[i]) == &config, "findWorkload finds " + names[i]);
        expect(config.family == WorkloadFamily::BEHAVIOR, names[i] + " is a behavior workload");
        validateWorkloadConfig(config);
    }

    BehaviorMix unbalanced;
    unbalanced.submitPercent = 90;
    WorkloadConfig bad = behaviorWorkloads()[0];
    bad.behavior = unbalanced;
    expectThrows([&bad] { validateWorkloadConfig(bad); }, "a command mix that does not sum to 100% is refused");
    bad.behavior = BehaviorMix{};
    bad.behavior.passivePercent = 50;
    expectThrows([&bad] { validateWorkloadConfig(bad); }, "so is a submission mix");
    bad.behavior = BehaviorMix{};
    bad.behavior.topOwners = 2;
    expectThrows([&bad] { validateWorkloadConfig(bad); }, "and top owners outside the pool");

    for (const WorkloadConfig& config : behaviorWorkloads()) {
        const Workload first = generateWorkload(config, 1500, 7);
        const Workload second = generateWorkload(config, 1500, 7);
        expect(first.behaviorTimed == second.behaviorTimed && first.expected == second.expected,
               config.name + ": the same seed gives the same commands and outcome");
        expect(first.timed.empty() && first.preload.empty(), config.name + ": only behavior commands");
        expect(first.behaviorTimed.size() == 1500 && first.timedCount() == 1500, config.name + ": timed command count");
        expect(first.expected.rejections == 0, config.name + ": nothing generated is refused");
        expect(first.expected.submits + first.expected.cancels + first.expected.replaces == 1500,
               config.name + ": every command counted");
        if (config.behavior.replaysWorkload.empty()) {
            expect(generateWorkload(config, 1500, 8).behaviorTimed != first.behaviorTimed,
                   config.name + ": another seed gives other commands");
        }
    }
}

// Each behavior workload exercises the behavior it is named for.
void testBehaviorTargets() {
    const auto smoke = [](const std::string& name) {
        return generateWorkload(findWorkload(name), timedCommandCount(BenchmarkSize::SMOKE), 42);
    };
    const Workload quotes = smoke("quote-refresh");
    expect(quotes.expected.replaces >= 5500 && quotes.expected.expirations > 0,
           "quote-refresh mostly replaces, and some post-only quotes would take");
    for (const BehaviorCommand& command : quotes.behaviorPreload) {
        expect(command.request.postOnly, "quote-refresh's preload is post-only quotes");
    }
    const Workload stops = smoke("pending-stops");
    expect(stops.expected.stopsTriggered >= 1000, "pending-stops triggers stops");
    expect(stops.expected.finalPendingStops >= 4000 && stops.expected.finalPendingStops <= 6000,
           "and keeps about 5,000 pending");
    const Workload owned = smoke("self-trade");
    expect(owned.expected.preventions >= 500, "self-trade prevents self-trades");
    const Workload immediate = smoke("immediate");
    expect(immediate.expected.expirations >= 500, "immediate orders expire what they cannot fill");

    // matching-heavy-request is matching-heavy's stream exactly, owner 0; -owned gives each
    // order its own ID as owner, so nothing is prevented and the outcome is the same.
    const Workload v1 = smoke("matching-heavy");
    for (const std::string name : {"matching-heavy-request", "matching-heavy-owned"}) {
        const Workload request = smoke(name);
        const bool ownedStream = name == "matching-heavy-owned";
        expect(request.behaviorPreload.size() == v1.preload.size() && request.behaviorTimed.size() == v1.timed.size(),
               name + " has matching-heavy's commands");
        for (std::size_t i = 0; i < v1.timed.size(); ++i) {
            const BenchmarkCommand& original = v1.timed[i];
            const BehaviorCommand& command = request.behaviorTimed[i];
            const OrderId expectedOwner = ownedStream && original.type == CommandType::SUBMIT ? original.orderId : 0;
            expect(command.type == original.type && command.request.id == original.orderId &&
                       (original.type == CommandType::CANCEL ||
                        (command.request.side == original.side && command.request.price == original.price &&
                         command.request.quantity == original.quantity)) &&
                       command.request.owner == expectedOwner,
                   name + ": command " + std::to_string(i) + " is matching-heavy's");
        }
        WorkloadOutcome v1Counters = request.expected;
        expect(v1Counters == v1.expected, name + " predicts matching-heavy's outcome");
    }
}

void testPreloadShape() {
    for (const WorkloadConfig& config : builtInWorkloads()) {
        const Workload workload = generateWorkload(config, 0, 42);
        expect(workload.preload.size() == config.initialOrders, config.name + ": preload size");
        expect(workload.timed.empty(), config.name + ": no timed commands asked for");

        std::set<Price> bidPrices;
        std::set<Price> askPrices;
        for (const BenchmarkCommand& command : workload.preload) {
            expect(command.type == CommandType::SUBMIT, config.name + ": the preload only submits");
            expect(command.quantity >= 1 && command.quantity <= 1000, config.name + ": preload quantity in range");
            if (command.side == Side::BUY) {
                expect(command.price < config.midPrice, config.name + ": preload bids below the middle");
                bidPrices.insert(command.price);
            } else {
                expect(command.price > config.midPrice, config.name + ": preload asks above the middle");
                askPrices.insert(command.price);
            }
        }
        expect(bidPrices.size() + askPrices.size() == config.targetLevels, config.name + ": target levels preloaded");
        expect(bidPrices.size() == (config.targetLevels + 1) / 2, config.name + ": half the levels are bids");
        expect(workload.expected == WorkloadOutcome{0, 0, 0, 0, 0, config.initialOrders, config.targetLevels},
               config.name + ": with no timed commands the book ends as preloaded");
    }
}

void testTimedCommands() {
    for (const WorkloadConfig& config : builtInWorkloads()) {
        const std::size_t count = timedCommandCount(BenchmarkSize::SMOKE);
        const Workload workload = generateWorkload(config, count, 42);
        expect(workload.timed.size() == count, config.name + ": timed command count");
        expect(workload.expected.submits + workload.expected.cancels == count, config.name + ": commands counted");
        expect(workload.expected.rejections == 0, config.name + ": nothing generated is refused");

        // Fresh, increasing IDs for every submission, continuing from the preload.
        OrderId lastId = static_cast<OrderId>(config.initialOrders);
        for (const BenchmarkCommand& command : workload.timed) {
            if (command.type == CommandType::SUBMIT) {
                expect(command.orderId == lastId + 1, config.name + ": submissions take the next fresh ID");
                lastId = command.orderId;
                expect(command.quantity >= 1 && command.quantity <= 1000, config.name + ": quantity in range");
            } else {
                expect(command.orderId >= 1 && command.orderId <= lastId, config.name + ": cancels a known ID");
                expect(command.price == 0 && command.quantity == 0, config.name + ": a cancel names only an ID");
            }
        }

        const double target = config.cancelPercent / 100.0;
        const double realised = workload.expected.cancellationRate();
        expect(realised <= target + 0.02 && realised >= target - 0.05,
               config.name + ": realised cancellation rate " + formatFixed(realised, 3) + " near " +
                   formatFixed(target, 3));
    }
}

void testPriceModes() {
    WorkloadConfig passive = findWorkload("small-book");
    passive.uniformPercent = 50;
    passive.concentratedPercent = 50;
    passive.marketablePercent = 0;
    passive.activeOrderCap = 0;
    const Workload resting = generateWorkload(passive, 5000, 3);
    expect(resting.expected.trades == 0, "with no marketable orders nothing trades");
    const Price low = passive.midPrice - (passive.targetLevels + 1) / 2;
    const Price high = passive.midPrice + passive.targetLevels / 2;
    for (const BenchmarkCommand& command : resting.timed) {
        if (command.type == CommandType::SUBMIT) {
            expect(command.price >= low && command.price <= high, "resting prices stay within the band");
        }
    }
    expect(resting.expected.finalPriceLevels <= passive.targetLevels, "and the book never has more levels");

    WorkloadConfig concentrated = passive;
    concentrated.uniformPercent = 0;
    concentrated.concentratedPercent = 100;
    concentrated.cancelPercent = 0;
    concentrated.concentratedTicks = 0;
    const Workload touch = generateWorkload(concentrated, 500, 3);
    for (const BenchmarkCommand& command : touch.timed) {
        const Price best = command.side == Side::BUY ? passive.midPrice - 1 : passive.midPrice + 1;
        expect(command.price == best, "concentrated orders with no spread join their own best");
    }

    WorkloadConfig aggressive = passive;
    aggressive.uniformPercent = 0;
    aggressive.concentratedPercent = 0;
    aggressive.marketablePercent = 100;
    aggressive.cancelPercent = 0;
    const Workload marketable = generateWorkload(aggressive, 50, 3);
    expect(marketable.expected.trades >= 50, "every marketable order trades while the other side has orders");
}

void testActiveOrderCap() {
    WorkloadConfig capped = findWorkload("small-book");
    capped.cancelPercent = 0;
    capped.uniformPercent = 50;
    capped.concentratedPercent = 50;
    capped.marketablePercent = 0;
    capped.activeOrderCap = 150;
    const Workload workload = generateWorkload(capped, 3000, 9);
    expect(workload.expected.finalActiveOrders <= 150, "submissions at the cap trade instead of resting");
    expect(workload.expected.trades > 0, "so even a mix with no marketable orders trades");
    expect(workload.expected.cancels == 0, "and the cancellation rate is untouched");

    capped.activeOrderCap = 0;
    expect(generateWorkload(capped, 3000, 9).expected.finalActiveOrders == 100 + 3000, "no cap, no limit");
}

// Every built-in workload, scaled down, run on the real book in both kinds: runOnce throws if
// the book's outcome differs from the model's prediction.
void testEngineAgreesWithModel() {
    std::vector<WorkloadConfig> all = builtInWorkloads();
    all.insert(all.end(), behaviorWorkloads().begin(), behaviorWorkloads().end());
    for (const WorkloadConfig& config : all) {
        const Workload workload = generateWorkload(scaledDown(config), 2000, 42);
        for (const BenchmarkKind kind : {BenchmarkKind::CORE, BenchmarkKind::REPLAY}) {
            const Measurement measurement = runOnce(workload, kind);
            expect(measurement.outcome == workload.expected, config.name + ": the book does what was predicted");
            expect(measurement.commands == 2000, config.name + ": every timed command counted");
            expect(measurement.elapsedNanoseconds >= 0, config.name + ": a non-negative time");
            expect(measurement.workload == config.name && measurement.kind == kind, config.name + ": labelled");
        }
    }
}

void testMismatchIsReported() {
    Workload workload = generateWorkload(scaledDown(findWorkload("matching-heavy")), 500, 1);
    expect(workload.expected.trades > 0, "the matching workload trades");
    workload.expected.trades += 1;
    expectThrows([&] { runOnce(workload, BenchmarkKind::CORE); }, "a core run that differs from its prediction throws");
    expectThrows([&] { runOnce(workload, BenchmarkKind::REPLAY); }, "so does a replay run");
}

void testRepetitions() {
    const Workload workload = generateWorkload(findWorkload("small-book"), 300, 5);
    RunSettings settings;
    settings.warmups = 2;
    settings.repetitions = 3;
    const std::vector<Measurement> measurements = runRepeated(workload, BenchmarkKind::CORE, settings);
    expect(measurements.size() == 5, "every warm-up and repetition is kept");
    const std::vector<std::pair<bool, std::size_t>> expected{{true, 1}, {true, 2}, {false, 1}, {false, 2}, {false, 3}};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        expect(measurements[i].warmup == expected[i].first, "warm-ups come first");
        expect(measurements[i].repetition == expected[i].second, "repetitions are numbered from 1 per phase");
        expect(measurements[i].outcome == workload.expected, "each repetition starts from a fresh book");
    }

    settings.warmups = 0;
    settings.repetitions = 1;
    expect(runRepeated(workload, BenchmarkKind::REPLAY, settings).size() == 1, "no warm-up asked for, none run");
}

void testRates() {
    expect(operationsPerSecond(1000, 1000000) == 1e6, "1000 commands in 1 ms is 1M ops/s");
    expect(operationsPerSecond(1000, 0) == 0.0, "nothing timed gives no rate");
    const Measurement measurement = fabricated("w", BenchmarkKind::CORE, false, 1, 3000000000);
    expect(measurement.operationsPerSecond() == 200.0, "600 commands in 3 s is 200 ops/s");
    expect(measurement.nanosecondsPerCommand() == 5000000.0, "and 5 ms each");
    expect(measurement.outcome.cancellationRate() > 0.333 && measurement.outcome.cancellationRate() < 0.334,
           "200 cancels in 600 commands");
    expect(WorkloadOutcome{}.cancellationRate() == 0.0, "no commands, no cancellation rate");
}

void testSummaries() {
    // 600 commands at 100, 200, and 300 ops/s, around a warm-up and another workload.
    const std::vector<Measurement> measurements{
        fabricated("a", BenchmarkKind::CORE, true, 1, 1000),
        fabricated("a", BenchmarkKind::CORE, false, 1, 6000000000),
        fabricated("b", BenchmarkKind::REPLAY, false, 1, 3000000000),
        fabricated("a", BenchmarkKind::CORE, false, 2, 2000000000),
        fabricated("a", BenchmarkKind::CORE, false, 3, 3000000000),
        fabricated("a", BenchmarkKind::REPLAY, true, 1, 1000),
    };
    const std::vector<MeasurementSummary> summaries = summarize(measurements);
    expect(summaries.size() == 2, "one summary per workload and kind with a measured run");
    const MeasurementSummary& a = summaries[0];
    expect(a.workload == "a" && a.kind == BenchmarkKind::CORE, "in order of first appearance");
    expect(a.repetitions == 3 && a.commands == 600, "warm-ups are left out");
    expect(a.minimum == 100.0 && a.maximum == 300.0, "minimum and maximum");
    expect(a.median == 200.0 && a.mean == 200.0, "median and mean");
    expect(a.standardDeviation > 99.999 && a.standardDeviation < 100.001, "sample standard deviation");
    expect(a.coefficientOfVariation > 0.49999 && a.coefficientOfVariation < 0.50001, "coefficient of variation");
    const MeasurementSummary& b = summaries[1];
    expect(b.workload == "b" && b.repetitions == 1 && b.standardDeviation == 0.0, "one run has no spread");

    const std::vector<Measurement> even{fabricated("c", BenchmarkKind::CORE, false, 1, 6000000000),
                                        fabricated("c", BenchmarkKind::CORE, false, 2, 3000000000)};
    expect(summarize(even)[0].median == 150.0, "an even count's median is the middle pair's mean");

    const std::string table = formatSummaryTable(summaries);
    expect(contains(table, "median ops/s") && contains(table, "200") && contains(table, "50.0%"),
           "the table shows the median rate and its spread");
}

BenchmarkReport fabricatedReport() {
    BenchmarkReport report;
    report.build.config = "Release";
    report.build.compiler = "Test \"compiler\"";
    report.build.ndebug = true;
    report.seed = 42;
    report.size = "custom";
    report.timedCommands = 600;
    report.settings.warmups = 1;
    report.settings.repetitions = 1;
    report.startedAtUnixSeconds = 1700000000;
    WorkloadRecord record;
    record.config = findWorkload("small-book");
    record.fingerprint = 0xabcULL;
    record.preloadOrders = 100;
    record.timedCommands = 600;
    record.reservedOrders = 173;
    report.workloads.push_back(record);
    report.measurements.push_back(fabricated("small-book", BenchmarkKind::CORE, true, 1, 1500000000));
    report.measurements.push_back(fabricated("small-book", BenchmarkKind::CORE, false, 1, 3000000000));
    return report;
}

void testCsvExport() {
    const std::string csv = formatMeasurementsCsv(fabricatedReport());
    const std::string expected =
        "workload,kind,size,seed,generator_version,build_config,optimized,repetition,warmup,commands,"
        "elapsed_ns,ops_per_second,ns_per_command,submits,cancels,cancellation_rate,rejections,trades,"
        "volume,final_active_orders,final_price_levels,replaces,expirations,preventions,stops_triggered,"
        "final_pending_stops\n"
        "small-book,core,custom,42,3,Release,true,1,true,600,1500000000,400.000,2500000.000,400,200,0.333333,0,50,"
        "700,90,12,0,0,0,0,0\n"
        "small-book,core,custom,42,3,Release,true,1,false,600,3000000000,200.000,5000000.000,400,200,0.333333,0,50,"
        "700,90,12,0,0,0,0,0\n";
    expect(csv == expected, "CSV export:\n" + csv);
}

void testJsonExport() {
    const std::string json = formatReportJson(fabricatedReport());
    for (const std::string part :
         {"\"generator_version\": 3", "\"seed\": 42", "\"size\": \"custom\"", "\"timed_commands\": 600",
          "\"started_at_unix_seconds\": 1700000000", "\"config\": \"Release\"",
          "\"compiler\": \"Test \\\"compiler\\\"\"", "\"optimized\": true", "\"name\": \"small-book\"",
          "\"fingerprint\": \"0xabc\"", "\"target_levels\": 10", "\"marketable\": 25", "\"active_order_cap\": 200", "\"reserved_orders\": 173", "\"warmup\": true",
          "\"elapsed_ns\": 3000000000", "\"ops_per_second\": 200.000", "\"repetitions\": 1",
          "\"median\": 200.000", "\"stops_triggered\": 0", "\"final_pending_stops\": 0"}) {
        expect(contains(json, part), "JSON export holds " + part + ":\n" + json);
    }
    expect(std::count(json.begin(), json.end(), '{') == std::count(json.begin(), json.end(), '}'), "balanced braces");
    expect(std::count(json.begin(), json.end(), '[') == std::count(json.begin(), json.end(), ']'), "balanced brackets");
    expect(json.back() == '\n', "ends with a newline");

    BenchmarkReport empty;
    expect(contains(formatReportJson(empty), "\"measurements\": []"), "no measurements is an empty array");
}

void testReplayCsv() {
    const std::vector<BenchmarkCommand> commands{
        {CommandType::SUBMIT, 7, Side::BUY, 100, 5},
        {CommandType::SUBMIT, 8, Side::SELL, 101, 6},
        {CommandType::CANCEL, 7, Side::BUY, 0, 0},
    };
    const std::string csv = renderReplayCsv(commands);
    expect(csv == "sequence,command,order_id,side,price,quantity\n"
                  "1,ADD,7,BUY,100,5\n"
                  "2,ADD,8,SELL,101,6\n"
                  "3,CANCEL,7,,,\n",
           "replay CSV:\n" + csv);

    std::istringstream input(csv);
    const order_book::replay::ReplayInput parsed = order_book::replay::readReplayInput(input);
    expect(!parsed.error && parsed.records.size() == 3, "the parser reads every rendered record");
    const auto* add = std::get_if<order_book::replay::AddCommand>(&parsed.records[1].command);
    expect(add != nullptr && add->orderId == 8 && add->side == Side::SELL && add->price == 101 && add->quantity == 6,
           "as the commands they came from");
    expect(std::holds_alternative<order_book::replay::CancelCommand>(parsed.records[2].command), "cancel parsed");
}

// A behavior workload's commands render as v2 rows that parse back to the same requests.
void testBehaviorReplayCsv() {
    using order_book::OrderRequest;
    using order_book::OrderType;
    using order_book::ReplaceRequest;
    using order_book::SelfTradePrevention;
    using order_book::TimeInForce;
    std::vector<BehaviorCommand> commands(5);
    commands[0].request = OrderRequest{7, Side::BUY, 100, 5};
    commands[0].request.postOnly = true;
    commands[1].request = OrderRequest{8, Side::SELL, 0, 6, OrderType::MARKET, TimeInForce::IOC};
    commands[1].request.owner = 3;
    commands[1].request.selfTradePrevention = SelfTradePrevention::CANCEL_BOTH;
    commands[2].request = OrderRequest{9, Side::BUY, 0, 4, OrderType::STOP, TimeInForce::IOC, 105};
    commands[3].type = CommandType::REPLACE;
    commands[3].replacement = ReplaceRequest{7, 99, 8, false, 2, SelfTradePrevention::CANCEL_INCOMING};
    commands[4].type = CommandType::CANCEL;
    commands[4].request.id = 9;
    const std::string csv = renderReplayCsv(commands);
    expect(csv == std::string(order_book::replay::expectedHeaderV2) + "\n" +
                      "1,ADD,7,BUY,100,5,LIMIT,GTC,,Y,,\n"
                      "2,ADD,8,SELL,,6,MARKET,IOC,,,3,CANCEL_BOTH\n"
                      "3,ADD,9,BUY,,4,STOP,IOC,105,,,\n"
                      "4,REPLACE,7,,99,8,,,,,2,CANCEL_INCOMING\n"
                      "5,CANCEL,9,,,,,,,,,\n",
           "v2 replay CSV:\n" + csv);

    std::istringstream input(csv);
    const order_book::replay::ReplayInput parsed = order_book::replay::readReplayInput(input);
    expect(!parsed.error && parsed.records.size() == 5, "the parser reads every rendered v2 record");
    for (std::size_t i = 0; i < 3; ++i) {
        const OrderRequest& request = commands[i].request;
        const order_book::replay::AddCommand expected{request.id,          request.side,        request.price,
                                                      request.quantity,    request.type,        request.timeInForce,
                                                      request.stopPrice,   request.postOnly,    request.owner,
                                                      request.selfTradePrevention};
        expect(parsed.records[i].command == order_book::replay::Command{expected},
               "ADD " + std::to_string(i + 1) + " round-trips");
    }
    const ReplaceRequest& replacement = commands[3].replacement;
    const order_book::replay::ReplaceCommand expectedReplace{replacement.id,      replacement.price,
                                                             replacement.quantity, replacement.postOnly,
                                                             replacement.owner,    replacement.selfTradePrevention};
    expect(parsed.records[3].command == order_book::replay::Command{expectedReplace}, "REPLACE round-trips");
    expect(std::holds_alternative<order_book::replay::CancelCommand>(parsed.records[4].command), "cancel parsed");
}

void testFormatting() {
    expect(formatFixed(1.5, 3) == "1.500", "fixed decimals");
    expect(formatFixed(2.0 / 3.0, 2) == "0.67", "rounded");
    expect(formatFixed(1234567.0, 0) == "1234567", "no decimals");
}

void testBuildInfo() {
    BuildInfo build;
    build.ndebug = true;
    expect(build.optimized(), "NDEBUG without checks or sanitizers is optimized");
    build.invariantChecks = true;
    expect(!build.optimized(), "automatic invariant checks are not");
    build.invariantChecks = false;
    build.addressSanitizer = true;
    expect(!build.optimized(), "nor is AddressSanitizer");
    build.addressSanitizer = false;
    build.undefinedBehaviorSanitizer = true;
    expect(!build.optimized(), "nor is UBSan");
    expect(!BuildInfo{}.optimized(), "nor a build with assertions");

    const BuildInfo current = currentBuild();
    expect(current.invariantChecks == order_book::invariantChecksAfterCommands, "reports the book's own setting");
    expect(!current.config.empty() && !current.compiler.empty(), "names the configuration and compiler");
}

}  // namespace

// The capacity hint is the preload for a steady book and the predicted final size for a
// growing one.
void testExpectedBookSize() {
    const Workload steady = generateWorkload(findWorkload("cancellation-heavy"), 2000, 42);
    expect(expectedBookSize(steady) == std::max(steady.preload.size(), steady.expected.finalActiveOrders),
           "the hint is the larger of the preload and the final size");
    const Workload growing = generateWorkload(findWorkload("mostly-resting"), 2000, 42);
    expect(growing.expected.finalActiveOrders > growing.preload.size(), "mostly-resting grows its book");
    expect(expectedBookSize(growing) == growing.expected.finalActiveOrders, "a growing book is sized for its final size");
}

int main() {
    testSizes();
    testQuantityRanges();
    testBuiltInWorkloads();
    testConfigValidation();
    testDeterminism();
    testPinnedFingerprints();
    testV1WorkloadsUnchanged();
    testBehaviorWorkloads();
    testBehaviorTargets();
    testPreloadShape();
    testTimedCommands();
    testPriceModes();
    testActiveOrderCap();
    testEngineAgreesWithModel();
    testMismatchIsReported();
    testRepetitions();
    testRates();
    testSummaries();
    testCsvExport();
    testJsonExport();
    testExpectedBookSize();
    testReplayCsv();
    testBehaviorReplayCsv();
    testFormatting();
    testBuildInfo();

    std::cout << "benchmark harness tests passed" << std::endl;
    return EXIT_SUCCESS;
}
