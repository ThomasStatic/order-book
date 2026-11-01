#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "clock.hpp"
#include "histogram.hpp"
#include "latency_report.hpp"
#include "noise_floor.hpp"
#include "timetable.hpp"

namespace {

using namespace order_book::latency;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

// A value below 256 has a bucket of its own; above that a bucket is 1/128 of its lower edge.
void testBucketEdges() {
    for (const std::uint64_t value : {0ULL, 1ULL, 127ULL, 128ULL, 255ULL}) {
        expect(Histogram::bucketLow(Histogram::bucketOf(value)) == value, "values below 256 are exact");
        expect(Histogram::bucketHigh(Histogram::bucketOf(value)) == value, "values below 256 are exact");
    }
    // 256 to 511 step by 2, 512 to 1023 by 4: hand-worked from 128 sub-buckets per doubling.
    expect(Histogram::bucketLow(Histogram::bucketOf(256)) == 256, "256 opens a bucket");
    expect(Histogram::bucketHigh(Histogram::bucketOf(256)) == 257, "256's bucket is two wide");
    expect(Histogram::bucketOf(257) == Histogram::bucketOf(256), "257 shares 256's bucket");
    expect(Histogram::bucketOf(258) == Histogram::bucketOf(256) + 1, "258 opens the next bucket");
    expect(Histogram::bucketLow(Histogram::bucketOf(511)) == 510, "511 closes 510's bucket");
    expect(Histogram::bucketOf(512) == Histogram::bucketOf(511) + 1, "a doubling's first bucket follows the last");
    expect(Histogram::bucketLow(Histogram::bucketOf(1023)) == 1020, "1023 sits in a bucket four wide");
    expect(Histogram::bucketHigh(Histogram::bucketOf(1023)) == 1023, "1023 closes its bucket");
    const std::uint64_t largest = ~std::uint64_t{0};
    expect(Histogram::bucketOf(largest) + 1 == Histogram::bucketCount, "the largest value takes the last bucket");
    expect(Histogram::bucketHigh(Histogram::bucketCount - 1) == largest, "the last bucket ends at the largest value");

    Histogram histogram;
    histogram.record(0);
    histogram.record(largest);
    expect(histogram.count() == 2, "both extremes are counted");
    expect(histogram.maximum() == largest, "the maximum is exact");
}

// A percentile from the histogram agrees with the sorted samples to within one bucket.
void testPercentilesAgainstSortedSamples() {
    std::mt19937_64 random(12345);
    std::vector<std::uint64_t> samples;
    Histogram histogram;
    for (int i = 0; i < 200000; ++i) {
        const std::uint64_t value = (random() >> (random() % 40)) % 100000000 + 1;
        samples.push_back(value);
        histogram.record(value);
    }
    std::sort(samples.begin(), samples.end());
    for (const Percentile percentile : reportedPercentiles) {
        const std::uint64_t rank = rankOf(histogram.count(), percentile);
        const std::uint64_t exact = samples[static_cast<std::size_t>(rank - 1)];
        const std::uint64_t reported = histogram.valueAt(percentile);
        expect(reported >= exact, "a percentile never understates the sample at its rank");
        expect(static_cast<double>(reported - exact) <= static_cast<double>(exact) / 128.0 + 1.0,
               "a percentile stays within the histogram's precision");
    }
    expect(histogram.maximum() == samples.back(), "the maximum is the largest sample");
}

void testPercentilesStopAtTheMaximum() {
    Histogram histogram;
    histogram.record(1000);  // the bucket runs to 1003
    expect(histogram.valueAt(reportedPercentiles[0]) == 1000, "a percentile never passes the largest sample");
}

void testMerging() {
    Histogram first;
    Histogram second;
    Histogram together;
    for (std::uint64_t value = 1; value <= 3000; ++value) {
        (value % 2 == 0 ? first : second).record(value * 7);
        together.record(value * 7);
    }
    first.merge(second);
    expect(first == together, "merging equals recording everything in one histogram");
    expect(first.count() == 3000, "merging adds the counts");
    expect(first.maximum() == 21000, "merging keeps the larger maximum");
}

// 1,000 samples must lie beyond a percentile before it may print.
void testThousandBeyondRule() {
    const Percentile p999{99900, "p99.9"};
    expect(rankOf(1000000, p999) == 999000, "p99.9 of a million is the 999,000th sample");
    expect(samplesBeyond(1000000, p999) == 1000, "a million samples leave 1,000 beyond p99.9");
    expect(printable(1000000, p999), "1,000 beyond prints");
    expect(!printable(999999, p999), "999 beyond does not");
    const Percentile p50{50000, "p50"};
    expect(printable(2000, p50), "2,000 samples leave 1,000 beyond p50");
    expect(!printable(1999, p50), "1,999 samples do not");
    expect(rankOf(1, p50) == 1, "a rank rounds up");
    expect(rankOf(0, p50) == 0, "no samples, no rank");
}

void testCountsListOccupiedBucketsOnly() {
    Histogram histogram;
    histogram.record(5);
    histogram.record(5);
    histogram.record(300);
    expect(histogram.formatCounts() == "low,high,count\n5,5,2\n300,301,1\n", "raw counts list each occupied bucket");
}

// A clock that only moves when told to. Spinning jumps straight to the deadline.
class FakeClock {
public:
    Ticks now() const { return time_; }
    Ticks spinUntil(Ticks deadline) {
        time_ = std::max(time_, deadline);
        return time_;
    }
    void advance(Ticks ticks) { time_ += ticks; }

private:
    Ticks time_ = 0;
};

// A target that takes cost(index) ticks and remembers when each command really went out.
template <typename Cost>
class FakeTarget {
public:
    FakeTarget(FakeClock& clock, Cost cost) : clock_(clock), cost_(cost) {}
    Ticks operator()(std::size_t index, FakeClock&) {
        sends.push_back(clock_.now());
        clock_.advance(cost_(index));
        return clock_.now();
    }
    std::vector<Ticks> sends;

private:
    FakeClock& clock_;
    Cost cost_;
};

constexpr Percentile p50{50000, "p50"};
constexpr Percentile p75{75000, "p75"};
constexpr Percentile p9999{99990, "p99.99"};

void expectWithinPrecision(std::uint64_t actual, std::uint64_t expected, const std::string& message) {
    const double difference = static_cast<double>(actual > expected ? actual - expected : expected - actual);
    expect(difference <= static_cast<double>(expected) / 128.0 + 1.0, message);
}

void testSendsWaitForTheirTimeAndLateOnesGoStraightOut() {
    FakeClock clock;
    // Each scheduled 100 apart; the second takes 250, so the third goes out 150 late.
    FakeTarget target(clock, [](std::size_t index) { return index == 1 ? Ticks{250} : Ticks{10}; });
    const std::vector<Ticks> schedule{0, 100, 200, 300, 400};
    const TimetableRun run = runTimetable(schedule, clock, target);
    expect((target.sends == std::vector<Ticks>{0, 100, 350, 360, 400}), "late sends go straight out, with none skipped");
    expect(run.lateness.count() == 5, "every send records its lateness");
    expect(run.lateness.maximum() == 150, "lateness runs from the scheduled send");
    expect(run.response.maximum() == 250, "response time runs from the scheduled send to the return");
    expect(run.service.maximum() == 250, "service time runs from the real send to the return");
    expect(run.elapsed == 410, "elapsed runs from the start to the last return");
}

void testScheduleIsRelativeToTheStart() {
    FakeClock clock;
    clock.advance(1000);
    FakeTarget target(clock, [](std::size_t) { return Ticks{1}; });
    const TimetableRun run = runTimetable(std::vector<Ticks>{0, 5}, clock, target);
    expect(target.sends.front() == 1000, "a schedule is relative to the run's start");
    expect(target.sends.back() == 1005, "a schedule is relative to the run's start");
    expect(run.lateness.maximum() == 0, "a send on time is not late");
}

std::vector<Ticks> fixedSchedule(std::size_t count, Ticks gap) {
    std::vector<Ticks> schedule;
    for (std::size_t i = 0; i < count; ++i) {
        schedule.push_back(static_cast<Ticks>(i) * gap);
    }
    return schedule;
}

void testBacklogStillGrowingIsNotSustained() {
    {
        FakeClock clock;
        FakeTarget target(clock, [](std::size_t) { return Ticks{200}; });
        expect(!runTimetable(fixedSchedule(1000, 100), clock, target).sustained,
               "a target slower than the timetable falls further behind");
    }
    {
        FakeClock clock;
        FakeTarget target(clock, [](std::size_t) { return Ticks{50}; });
        expect(runTimetable(fixedSchedule(1000, 100), clock, target).sustained, "a target that keeps up is sustained");
    }
    {
        // One stall early on, drained well before the end, leaves no growing backlog.
        FakeClock clock;
        FakeTarget target(clock, [](std::size_t index) { return index == 100 ? Ticks{5000} : Ticks{50}; });
        expect(runTimetable(fixedSchedule(1000, 100), clock, target).sustained, "a stall that drains is sustained");
    }
}

// Gil Tene's worked example, from "How NOT to Measure Latency". A server answers in 1 ms, at
// 100 commands a second, for 100 s, then freezes for 100 s. One tick is 1 us here. Measured
// properly, half the commands see the freeze: p50 is 1 ms, p75 is 50 s, and p99.99 is about
// 100 s. A closed-loop client sends nothing during the freeze, so it records 10,000 samples
// of 1 ms and one of 100 s, and p99.99 still reads 1 ms.
void testCoordinatedOmissionWorkedExample() {
    const Ticks millisecond = 1000;
    const Ticks second = 1000 * millisecond;
    const auto cost = [=](std::size_t index) {
        if (index < 10000) {
            return millisecond;
        }
        return index == 10000 ? 100 * second : Ticks{0};
    };

    FakeClock clock;
    FakeTarget target(clock, cost);
    const TimetableRun open = runTimetable(fixedSchedule(20000, 10 * millisecond), clock, target);
    expect(open.response.count() == 20000, "no command is skipped");
    expectWithinPrecision(open.response.valueAt(p50), 1 * millisecond, "open loop: p50 is 1 ms");
    expectWithinPrecision(open.response.valueAt(p75), 50 * second, "open loop: p75 is 50 s");
    expectWithinPrecision(open.response.valueAt(p9999), 100 * second, "open loop: p99.99 is about 100 s");
    expect(open.response.maximum() == 100 * second, "open loop: the worst command waited out the whole freeze");

    FakeClock closedClock;
    FakeTarget closedTarget(closedClock, cost);
    const Histogram closed = runClosedLoop(10001, closedClock, closedTarget);
    expect(closed.count() == 10001, "closed loop: only 10,001 commands were ever sent");
    expectWithinPrecision(closed.valueAt(p50), 1 * millisecond, "closed loop: p50 is 1 ms");
    expectWithinPrecision(closed.valueAt(p9999), 1 * millisecond, "closed loop: the freeze has vanished from p99.99");
    expect(closed.maximum() == 100 * second, "closed loop: only the maximum shows it");
}

void testPoissonScheduleIsSeededAndKeepsItsRate() {
    const std::vector<Ticks> first = poissonSchedule(100000, 100.0, 7);
    expect(first == poissonSchedule(100000, 100.0, 7), "the same seed gives the same timetable");
    expect(first != poissonSchedule(100000, 100.0, 8), "another seed gives another timetable");
    expect(std::is_sorted(first.begin(), first.end()), "a timetable never goes backwards");
    expect(first.front() == 0, "a timetable starts at once");
    const double meanGap = static_cast<double>(first.back()) / 99999.0;
    expect(meanGap > 95.0 && meanGap < 105.0, "the mean gap is the one asked for");
}

void testFixedScheduleKeepsFractionalGaps() {
    expect((fixedRateSchedule(4, 2.5) == std::vector<Ticks>{0, 2, 5, 7}), "a fixed timetable accumulates no rounding");
}

// A clock that returns a script, then keeps returning its last value.
class ScriptedClock {
public:
    explicit ScriptedClock(std::vector<Ticks> reads) : reads_(std::move(reads)) {}
    Ticks now() {
        const Ticks value = reads_[std::min(next_, reads_.size() - 1)];
        ++next_;
        return value;
    }

private:
    std::vector<Ticks> reads_;
    std::size_t next_ = 0;
};

void testNoiseFloorCountsGapsByThreshold() {
    // One tick per ns. Gaps of 500 ns (under every threshold), 2 us, 20 us, 200 us, and the
    // 777.5 us that ends the script.
    ScriptedClock clock({0, 500, 2500, 22500, 222500, 1000000});
    const NoiseFloor floor = measureNoiseFloor(clock, 1000000, 1.0);
    expect(floor.seconds == 0.001, "the floor knows how long it ran");
    expect(floor.gapsOver[0] == 4, "four gaps over 1 us");
    expect(floor.gapsOver[1] == 3, "three gaps over 10 us");
    expect(floor.gapsOver[2] == 2, "two gaps over 100 us");
    expect(floor.maximumNanoseconds == 777500.0, "the largest gap");
    expect(floor.lostShare == 0.9995, "the share of the time inside gaps over 1 us");
    expect(floor.gapsPerSecond(1) == 3000.0, "gaps per second");
}

// Only sanity: no test asserts a latency value.
void testClockMovesForwardAndCalibrates() {
    const Clock clock = detectClock();
    Ticks last = clock.now();
    for (int i = 0; i < 1000; ++i) {
        const Ticks next = clock.now();
        expect(next >= last, "the clock never goes backwards");
        last = next;
    }
    const ClockRecord record = measureClock(clock);
    expect(!record.name.empty(), "the clock names itself");
    expect(record.ticksPerNanosecond > 0.1 && record.ticksPerNanosecond < 100.0, "calibration lands in a broad band");
    expect(record.stepNanoseconds > 0.0, "a measured step");
    expect(record.readCostNanoseconds > 0.0, "a measured read cost");
    expect(record.emptyPairP99Nanoseconds >= record.emptyPairP50Nanoseconds, "p99 is at least p50");
}

std::string readExpected(const std::string& directory, const std::string& name) {
    // Text mode, so a checkout with CRLF line endings compares like one with LF.
    std::ifstream file(directory + "/expected/" + name);
    expect(static_cast<bool>(file), "missing expected file " + name);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void expectText(const std::string& actual, const std::string& directory, const std::string& name) {
    if (actual != readExpected(directory, name)) {
        std::cerr << "FAILED: " << name << " differs; got:\n" << actual << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

RunRecord completeRecord() {
    RunRecord record;
    for (const std::string& field : record.missing()) {
        record.set(field, "x");
    }
    record.set("interval", "command");
    record.set("time", "service");
    record.set("load", "closed-loop");
    record.set("workload", "matching-heavy");
    return record;
}

NoiseFloor quietFloor() {
    NoiseFloor floor;
    floor.seconds = 10.0;
    floor.gapsOver = {1200, 350, 10};
    floor.maximumNanoseconds = 540000.0;
    floor.lostShare = 0.002;
    return floor;
}

// Nine runs, hand-worked. Run k holds 1,000 samples of 100 + k, 800 of 120, and 200 of 200 + k,
// all under 256 so that each has a bucket of its own. Merged, the 9,000th of 18,000 sorted
// samples is 108 and the 16,200th is 120. The largest are 200 of 208.
ReportRow nineRunRow() {
    ReportRow row;
    row.title = "command, service time, matching-heavy, closed-loop";
    row.record = completeRecord();
    for (std::uint64_t run = 0; run < 9; ++run) {
        Histogram histogram;
        for (int i = 0; i < 1000; ++i) {
            histogram.record(100 + run);
        }
        for (int i = 0; i < 800; ++i) {
            histogram.record(120);
        }
        for (int i = 0; i < 200; ++i) {
            histogram.record(200 + run);
        }
        row.runs.push_back(histogram);
    }
    row.ticksPerNanosecond = 1.0;
    row.stepNanoseconds = 1.0;
    row.emptyPairP50Nanoseconds = 1.0;
    row.clockName = "rdtscp;lfence";
    row.noiseBefore = quietFloor();
    row.noiseAfter = quietFloor();
    row.commandsPerSecond = 1000.0;
    row.optimizedBuild = true;
    row.pinnedAtHighPriority = true;
    return row;
}

void testQuotableReport(const std::string& directory) {
    const ReportRow row = nineRunRow();
    expect(unquotableReasons(row).empty(), "a complete row is quotable");
    expectText(formatRow(row), directory, "report_quotable.txt");
}

void testMissingFieldMakesARunUnquotable(const std::string& directory) {
    ReportRow row = nineRunRow();
    row.record = RunRecord();
    for (const std::string& field : row.record.missing()) {
        if (field != "machine" && field != "capacity-growths") {
            row.record.set(field, "x");
        }
    }
    row.runs.resize(3);
    row.optimizedBuild = false;
    row.clockName = "steady_clock";
    row.noiseAfter.reset();
    const std::vector<std::string> reasons = unquotableReasons(row);
    expect(reasons.size() == 6, "every reason is listed");
    expect(reasons[0] == "missing capacity-growths", "a missing field is never a silent default");
    expect(reasons[1] == "missing machine", "each missing field is named");
    expectText(formatRow(row), directory, "report_unquotable.txt");
}

// At 2,000 commands a second, p50's tail happens 1,000 times a second and p90's 200. The worse
// floor has 60 gaps a second over 1 us, and 60 * 4 reaches 200 but not 1,000, so only p90 is
// the machine's. The empty pair is 110 ns at a step of 1 ns, so 108 is unresolved and 120 is not.
void testPercentilesAreMarked(const std::string& directory) {
    ReportRow row = nineRunRow();
    row.commandsPerSecond = 2000.0;
    row.emptyPairP50Nanoseconds = 110.0;
    row.noiseBefore->gapsOver = {100, 10, 0};
    row.noiseAfter->gapsOver = {600, 60, 0};
    expectText(formatRow(row), directory, "report_marks.txt");
}

void testNotSustainedReplacesThePercentiles(const std::string& directory) {
    ReportRow row = nineRunRow();
    row.title = "command, response time, matching-heavy, fixed at 90%";
    row.openLoop = true;
    row.sustained = false;
    row.pinnedAtHighPriority = false;
    expectText(formatRow(row), directory, "report_not_sustained.txt");
    const std::vector<std::string> reasons = unquotableReasons(row);
    expect(reasons.size() == 2, "unpinned and not sustained each count");
    expect(reasons[0] == "unpinned", "an open-loop run needs pinned threads at high priority");
    expect(reasons[1] == "not sustained", "a rate whose backlog grew is no capability");
}

void testClosedLoopMayBeUnpinned() {
    ReportRow row = nineRunRow();
    row.pinnedAtHighPriority = false;
    expect(unquotableReasons(row).empty(), "closed-loop service time may be reported unpinned");
}

void testUnknownRecordFieldIsRefused() {
    RunRecord record;
    bool threw = false;
    try {
        record.set("no-such-field", "x");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "a misspelt field can't fill nothing and leave its real field missing unnoticed");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: latency_harness_tests <tests/latency directory>\n";
        return EXIT_FAILURE;
    }
    const std::string directory = argv[1];
    testBucketEdges();
    testPercentilesAgainstSortedSamples();
    testPercentilesStopAtTheMaximum();
    testMerging();
    testThousandBeyondRule();
    testCountsListOccupiedBucketsOnly();
    testSendsWaitForTheirTimeAndLateOnesGoStraightOut();
    testScheduleIsRelativeToTheStart();
    testBacklogStillGrowingIsNotSustained();
    testCoordinatedOmissionWorkedExample();
    testPoissonScheduleIsSeededAndKeepsItsRate();
    testFixedScheduleKeepsFractionalGaps();
    testNoiseFloorCountsGapsByThreshold();
    testClockMovesForwardAndCalibrates();
    testQuotableReport(directory);
    testMissingFieldMakesARunUnquotable(directory);
    testPercentilesAreMarked(directory);
    testNotSustainedReplacesThePercentiles(directory);
    testClosedLoopMayBeUnpinned();
    testUnknownRecordFieldIsRefused();
    std::cout << "latency_harness_tests passed\n";
    return EXIT_SUCCESS;
}
