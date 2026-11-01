#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A log-linear histogram of raw clock ticks: 128 sub-buckets per doubling, so a bucket is at
// most 1/128 (0.8%) of its value wide, and every value below 256 has a bucket of its own.
// Nothing is converted to time here. Raw counts are kept so that any percentile can be
// recomputed later, and histograms merge by adding counts, which is the only way to combine
// runs: a percentile of percentiles means nothing.
namespace order_book::latency {

// A percentile as parts per 100,000, so ranks are exact integers: 0.999 * 1,000,000 is not
// 999,000 in floating point.
struct Percentile {
    std::uint64_t partsPerHundredThousand;
    std::string_view name;
};

inline constexpr std::array<Percentile, 4> reportedPercentiles{
    Percentile{50000, "p50"}, Percentile{90000, "p90"}, Percentile{99000, "p99"}, Percentile{99900, "p99.9"}};

// A percentile prints only with this many samples beyond it, so p99.9 needs 1,000,000.
inline constexpr std::uint64_t minimumSamplesBeyond = 1000;

// The 1-based rank of the sample at a percentile of count samples, rounded up.
constexpr std::uint64_t rankOf(std::uint64_t count, Percentile percentile) {
    return (count * percentile.partsPerHundredThousand + 99999) / 100000;
}

constexpr std::uint64_t samplesBeyond(std::uint64_t count, Percentile percentile) {
    return count - rankOf(count, percentile);
}

constexpr bool printable(std::uint64_t count, Percentile percentile) {
    return samplesBeyond(count, percentile) >= minimumSamplesBeyond;
}

class Histogram {
public:
    // 64-bit values: 256 exact buckets, then 128 for each of the 56 further doublings.
    static constexpr std::size_t bucketCount = 256 + 56 * 128;

    static constexpr std::size_t bucketOf(std::uint64_t value) {
        if (value < 256) {
            return static_cast<std::size_t>(value);
        }
        const auto shift = static_cast<unsigned int>(std::bit_width(value)) - 8;
        return static_cast<std::size_t>(shift) * 128 + static_cast<std::size_t>(value >> shift);
    }
    static constexpr std::uint64_t bucketLow(std::size_t bucket) {
        if (bucket < 256) {
            return bucket;
        }
        const auto shift = static_cast<unsigned int>(bucket / 128 - 1);
        return static_cast<std::uint64_t>(bucket - std::size_t{shift} * 128) << shift;
    }
    static constexpr std::uint64_t bucketHigh(std::size_t bucket) {
        if (bucket < 256) {
            return bucket;
        }
        const auto shift = static_cast<unsigned int>(bucket / 128 - 1);
        return bucketLow(bucket) + ((std::uint64_t{1} << shift) - 1);
    }

    Histogram();

    void record(std::uint64_t value) {
        ++counts_[bucketOf(value)];
        ++count_;
        if (value > maximum_) {
            maximum_ = value;
        }
    }

    void merge(const Histogram& other);

    std::uint64_t count() const { return count_; }
    // The largest value recorded, exactly; 0 for an empty histogram.
    std::uint64_t maximum() const { return maximum_; }
    // The upper edge of the bucket holding the sample at this rank, so it never understates
    // that sample, but never above the maximum. 0 for an empty histogram.
    std::uint64_t valueAt(Percentile percentile) const;

    // "low,high,count\n", then one line per occupied bucket, lowest first.
    std::string formatCounts() const;

    bool operator==(const Histogram&) const = default;

private:
    std::vector<std::uint64_t> counts_;
    std::uint64_t count_ = 0;
    std::uint64_t maximum_ = 0;
};

}  // namespace order_book::latency
