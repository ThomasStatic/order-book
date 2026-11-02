#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace order_book::reference {

    // How commands are shared between instruments. Open to new spreads, each of which needs a
    // target check in interleaver_tests and its own pinned fingerprint.
    enum class Spread {
        UNIFORM,  // every instrument equally likely
        SKEWED,   // Zipf with exponent 1: instrument k takes a share of 1/(k+1), so 0 is the top
        ONE_HOT   // every command is instrument 0's; a test spread
    };

    // Chooses whose command comes next, from a source of its own, so that one instrument's
    // commands never depend on another's. Each instrument's stream stays one unchanged
    // generator, and a one-instrument run is exactly that generator's stream.
    //
    // Like RandomCommandGenerator it uses the raw output of std::mt19937_64 and its own unbiased
    // draw, and no std::*_distribution and no floating point, so a sequence is the same on
    // every compiler and platform.
    class Interleaver {
    public:
        // Throws std::invalid_argument for no instruments.
        Interleaver(std::uint64_t seed, std::size_t instruments, Spread spread);

        // The index of the instrument whose command comes next, below the instrument count.
        std::size_t next();

    private:
        std::uint64_t below(std::uint64_t bound);

        std::mt19937_64 source;
        std::size_t instruments;
        Spread spread;
        std::vector<std::uint64_t> cumulative;  // SKEWED's running total of weights, by instrument
    };

}  // namespace order_book::reference
