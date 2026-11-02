#include "interleaver.hpp"

#include <algorithm>
#include <stdexcept>

namespace order_book::reference {

    namespace {
        // A Zipf weight is this over the instrument's rank, in whole numbers.
        constexpr std::uint64_t zipfScale = std::uint64_t{1} << 40;
    }

    Interleaver::Interleaver(std::uint64_t seed, std::size_t count, Spread chosen)
        : source(seed), instruments(count), spread(chosen) {
        if (count == 0) {
            throw std::invalid_argument("an interleaver needs at least one instrument");
        }
        if (spread == Spread::SKEWED) {
            cumulative.reserve(count);
            std::uint64_t total = 0;
            for (std::size_t rank = 1; rank <= count; ++rank) {
                total += zipfScale / rank;
                cumulative.push_back(total);
            }
        }
    }

    std::size_t Interleaver::next() {
        switch (spread) {
            case Spread::UNIFORM:
                return static_cast<std::size_t>(below(instruments));
            case Spread::SKEWED: {
                const std::uint64_t draw = below(cumulative.back());
                const auto found = std::upper_bound(cumulative.begin(), cumulative.end(), draw);
                return static_cast<std::size_t>(found - cumulative.begin());
            }
            case Spread::ONE_HOT:
                return 0;
        }
        return 0;
    }

    std::uint64_t Interleaver::below(std::uint64_t bound) {
        // Refuses the raw values that would favor the low remainders: 2^64 mod bound of them.
        const std::uint64_t refused = (0 - bound) % bound;
        while (true) {
            const std::uint64_t raw = source();
            if (raw >= refused) {
                return raw % bound;
            }
        }
    }

}  // namespace order_book::reference
