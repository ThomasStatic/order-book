#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "interleaver.hpp"

namespace {

using order_book::reference::Interleaver;
using order_book::reference::Spread;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

std::vector<std::size_t> draws(std::uint64_t seed, std::size_t instruments, Spread spread, std::size_t count) {
    Interleaver interleaver(seed, instruments, spread);
    std::vector<std::size_t> indexes;
    indexes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        indexes.push_back(interleaver.next());
    }
    return indexes;
}

std::vector<std::size_t> counts(const std::vector<std::size_t>& indexes, std::size_t instruments) {
    std::vector<std::size_t> perInstrument(instruments, 0);
    for (const std::size_t index : indexes) {
        ++perInstrument.at(index);
    }
    return perInstrument;
}

std::uint64_t fingerprint(const std::vector<std::size_t>& indexes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const std::size_t index : indexes) {
        hash ^= index;
        hash *= 1099511628211ull;
    }
    return hash;
}

// Worked out apart from this code, from a reference MT19937-64 checked against the value the
// C++ standard gives for mt19937_64. A change here changes what every recorded seed means.
void testPinnedSequences() {
    using Indexes = std::vector<std::size_t>;
    expect(draws(42, 8, Spread::UNIFORM, 10) == Indexes{6, 0, 2, 6, 5, 4, 0, 0, 6, 1}, "uniform's first draws, 8 instruments");
    expect(draws(7, 32, Spread::UNIFORM, 10) == Indexes{7, 2, 14, 22, 29, 12, 1, 6, 1, 4}, "uniform's first draws, 32 instruments");
    expect(draws(42, 8, Spread::SKEWED, 10) == Indexes{4, 1, 0, 1, 3, 2, 6, 4, 0, 1}, "skewed's first draws, 8 instruments");
    expect(draws(7, 32, Spread::SKEWED, 10) == Indexes{10, 0, 0, 4, 7, 23, 1, 1, 0, 2}, "skewed's first draws, 32 instruments");
    expect(draws(42, 8, Spread::ONE_HOT, 10) == Indexes(10, 0), "one-hot's first draws");
}

void testPinnedFingerprints() {
    expect(fingerprint(draws(42, 8, Spread::UNIFORM, 10000)) == 0x3e4f066e23a1109cull, "uniform, 8 instruments");
    expect(fingerprint(draws(7, 32, Spread::UNIFORM, 10000)) == 0x252b67a83e4f40afull, "uniform, 32 instruments");
    expect(fingerprint(draws(42, 8, Spread::SKEWED, 10000)) == 0x5e99d7cd6663ade0ull, "skewed, 8 instruments");
    expect(fingerprint(draws(7, 32, Spread::SKEWED, 10000)) == 0x5eaae46bc5a82fabull, "skewed, 32 instruments");
    expect(fingerprint(draws(42, 8, Spread::ONE_HOT, 10000)) == 0xa6e4f0723147f065ull, "one-hot, 8 instruments");
    expect(fingerprint(draws(7, 32, Spread::ONE_HOT, 10000)) == 0xa6e4f0723147f065ull, "one-hot, 32 instruments");
}

void testASeedDeterminesTheSequence() {
    expect(draws(11, 16, Spread::SKEWED, 1000) == draws(11, 16, Spread::SKEWED, 1000), "the same seed, the same sequence");
    expect(draws(11, 16, Spread::SKEWED, 1000) != draws(12, 16, Spread::SKEWED, 1000), "another seed, another sequence");
}

// Zipf with exponent 1 over 32 instruments gives the top one 1 / H(32), about 24.6%, the second
// half of that, and the last a thirty-second of it.
void testSkewedHitsItsTarget() {
    const std::size_t total = 200000;
    const std::vector<std::size_t> perInstrument = counts(draws(3, 32, Spread::SKEWED, total), 32);
    const double top = static_cast<double>(perInstrument[0]) / static_cast<double>(total);
    const double second = static_cast<double>(perInstrument[1]) / static_cast<double>(total);
    const double last = static_cast<double>(perInstrument[31]) / static_cast<double>(total);
    expect(top > 0.238 && top < 0.254, "the top instrument of 32 takes about 25%, not " + std::to_string(top));
    expect(second > 0.115 && second < 0.131, "the second takes half of that, not " + std::to_string(second));
    expect(last > 0.0060 && last < 0.0094, "the last takes a thirty-second of that, not " + std::to_string(last));
}

void testUniformHitsItsTarget() {
    const std::size_t total = 200000;
    for (const std::size_t count : counts(draws(3, 32, Spread::UNIFORM, total), 32)) {
        expect(count > total / 32 * 9 / 10 && count < total / 32 * 11 / 10, "each of 32 takes about a thirty-second");
    }
}

void testOneHotHitsItsTarget() {
    const std::vector<std::size_t> perInstrument = counts(draws(3, 32, Spread::ONE_HOT, 1000), 32);
    expect(perInstrument[0] == 1000, "one instrument takes every command");
}

void testEdges() {
    expect(draws(1, 1, Spread::UNIFORM, 100) == std::vector<std::size_t>(100, 0), "one instrument under uniform");
    expect(draws(1, 1, Spread::SKEWED, 100) == std::vector<std::size_t>(100, 0), "one instrument under skewed");
    try {
        Interleaver interleaver(1, 0, Spread::UNIFORM);
        expect(false, "no instruments should be refused");
    } catch (const std::invalid_argument&) {
    }
}

}  // namespace

int main() {
    testPinnedSequences();
    testPinnedFingerprints();
    testASeedDeterminesTheSequence();
    testSkewedHitsItsTarget();
    testUniformHitsItsTarget();
    testOneHotHitsItsTarget();
    testEdges();
    std::cout << "interleaver tests passed" << std::endl;
    return EXIT_SUCCESS;
}
