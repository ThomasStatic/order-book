#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "command.hpp"
#include "interleaver.hpp"
#include "order_book/command_log.hpp"
#include "order_book/order_book.hpp"
#include "order_book/symbol.hpp"

// The engine's single-threaded harness: generated commands through the real entry check and
// Shard, beside one model and one witness book per instrument. See docs/internals/testing.md.
namespace order_book::engine_testing {

    using replay::Symbol;

    struct EngineSeedConfig {
        std::uint64_t seed = 1;
        std::size_t instruments = 8;
        std::uint32_t shards = 2;
        std::size_t commands = 5000;
        reference::Spread spread = reference::Spread::UNIFORM;
        // Each instrument's profile, cycling through these. Empty means every built-in one.
        std::vector<std::string> profiles;
        unsigned int symbolFaultPercent = 2;  // of commands, an unknown or a malformed symbol
        // The shard's books are warmed this little, and the witnesses this much, so every
        // comparison also shows capacity changes no outcome.
        BookCapacity shardCapacity{1, 1, 1};
        BookCapacity witnessCapacity{4096, 1024, 256};
        // For testing the harness: before this command, a shard's book takes one extra order its
        // witness and model never hear of.
        std::optional<std::size_t> plantDivergenceBefore;
    };

    enum class SymbolFault { NONE, UNKNOWN, MALFORMED };

    // One line of a stream.
    struct EngineCommand {
        std::size_t instrument;
        randomized::Command generated;  // as the generator made it
        std::optional<randomized::Command> converted;  // what reaches the engine, if anything
        SymbolFault fault = SymbolFault::NONE;
    };

    // What the seed produces, generated ahead of the run on the models alone, so nothing in the
    // run can change it. Instrument 0 uses the run's seed, so a one-instrument run is exactly the
    // single-book harness's stream. Each instrument has one unchanged generator and model, so one
    // instrument's commands cannot depend on another's, and every instrument's IDs start at 1.
    std::vector<EngineCommand> generateEngineStream(const EngineSeedConfig& config);

    // The command as it reaches the engine: a valid Order becomes its identical GTC limit
    // request, a mode with no owner is cleared, and what no entry point can express is dropped.
    std::optional<randomized::Command> convertForEngine(const randomized::Command& command);

    // The command's bytes as a client would send them, however malformed.
    replay::RawCommand rawCommandOf(const randomized::Command& command, const std::array<char, Symbol::size>& symbol);

    struct EngineFailure {
        std::size_t commandIndex;
        std::string instrument;
        std::string reason;
    };

    struct EngineStats {
        std::size_t queued = 0;
        std::size_t rejectedAtEntry = 0;
        std::size_t refused = 0;
        std::size_t dropped = 0;
        std::uint64_t shardGrowths = 0;    // shows the shard's books really were too small
        std::uint64_t witnessGrowths = 0;
    };

    struct EngineRunResult {
        std::optional<EngineFailure> failure;
        EngineStats stats;

        bool passed() const { return !failure.has_value(); }
    };

    EngineRunResult runEngineSeed(const EngineSeedConfig& config);

}  // namespace order_book::engine_testing
