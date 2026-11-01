#include "histogram.hpp"

#include <algorithm>

namespace order_book::latency {

Histogram::Histogram() : counts_(bucketCount, 0) {}

void Histogram::merge(const Histogram& other) {
    for (std::size_t bucket = 0; bucket < bucketCount; ++bucket) {
        counts_[bucket] += other.counts_[bucket];
    }
    count_ += other.count_;
    maximum_ = std::max(maximum_, other.maximum_);
}

std::uint64_t Histogram::valueAt(Percentile percentile) const {
    const std::uint64_t rank = rankOf(count_, percentile);
    std::uint64_t seen = 0;
    for (std::size_t bucket = 0; bucket < bucketCount; ++bucket) {
        seen += counts_[bucket];
        if (seen >= rank && seen != 0) {
            return std::min(bucketHigh(bucket), maximum_);
        }
    }
    return 0;
}

std::string Histogram::formatCounts() const {
    std::string text = "low,high,count\n";
    for (std::size_t bucket = 0; bucket < bucketCount; ++bucket) {
        if (counts_[bucket] != 0) {
            text += std::to_string(bucketLow(bucket)) + ',' + std::to_string(bucketHigh(bucket)) + ',' +
                    std::to_string(counts_[bucket]) + '\n';
        }
    }
    return text;
}

}  // namespace order_book::latency
