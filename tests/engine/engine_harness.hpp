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
// Shard, beside one model and one witness book per instrument. See
// docs/internals/randomized-tests.md.
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
        // And then cancels it, leaving only what the model can't see: the numbers it used.
        bool cancelPlantedOrder = false;
        // How often a LOG line follows a command, from a source of its own, so the commands are
        // the same whatever this is. 0 gives a stream of commands alone.
        unsigned int logWritePercent = 3;
        // Each shard's log ring: small, so a full one makes the shard wait for its log writer.
        std::size_t logRingRecords = 64;
        // For testing the harness: the first command queued at or after this one runs on its book
        // and its witness, but never reaches the log.
        std::optional<std::size_t> leaveUnloggedFrom;
    };

    enum class SymbolFault { NONE, UNKNOWN, MALFORMED };

    // A line is a command, or an engine action. LOG: each shard's log writer does one write
    // through the in-memory LogStorage, so until then commands are unwritten.
    enum class EngineAction { COMMAND, LOG };

    // One line of a stream. An action's other fields mean nothing.
    struct EngineCommand {
        std::size_t instrument;
        randomized::Command generated;  // as the generator made it
        std::optional<randomized::Command> converted;  // what reaches the engine, if anything
        SymbolFault fault = SymbolFault::NONE;
        EngineAction action = EngineAction::COMMAND;
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
        std::size_t malformedAtEntry = 0;
        std::size_t refused = 0;
        std::size_t dropped = 0;
        std::uint64_t shardGrowths = 0;    // shows the shard's books really were too small
        std::uint64_t witnessGrowths = 0;
        std::size_t logWrites = 0;         // LOG lines and full log rings that wrote anything
        std::size_t recordsLogged = 0;
    };

    struct EngineRunResult {
        std::optional<EngineFailure> failure;
        EngineStats stats;

        bool passed() const { return !failure.has_value(); }
    };

    // Every queued command goes to its shard's log ring, as the matching thread sends it, and its
    // log writer writes on LOG lines and whenever that ring is full. The run ends by stopping
    // each log writer, then replaying each shard's log per instrument through ReplayEngine, which
    // must give a book equal to the witness, and checking each Shard, durable position included.
    EngineRunResult runEngineSeed(const EngineSeedConfig& config);

}  // namespace order_book::engine_testing
