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
#include "order_book/runtime/durability.hpp"
#include "order_book/runtime/log_storage.hpp"
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
        // Every shard's log, which the crashes are measured against.
        runtime::Durability durability = runtime::Durability::MACHINE;
        // One or two CRASH lines, each after a command, from a source of their own, so the other
        // lines are the same whatever this is.
        bool crashes = true;
        // For testing the harness: the first crash also loses every file of shard 0's log but
        // its oldest, as a drive that lied about a flush would, and meets no write on its way.
        bool loseLogFilesAtFirstCrash = false;
        // How many RESTORE lines, each after a command and for an instrument picked from a source
        // of their own, so the other lines are the same whatever this is.
        std::size_t restores = 10;
    };

    enum class SymbolFault { NONE, UNKNOWN, MALFORMED };

    // A line is a command, or an engine action. LOG: each shard's log writer does one write
    // through the in-memory LogStorage, so until then commands are unwritten. CRASH_ENGINE and
    // CRASH_MACHINE <seed>: the in-memory LogStorage models that crash, then every shard
    // recovers and the run goes on. RESTORE <symbol>: that instrument's book is imaged and
    // restored, and the run goes on with the restored book (test-only: a running engine never
    // does this).
    enum class EngineAction { COMMAND, LOG, CRASH_ENGINE, CRASH_MACHINE, RESTORE };

    // One line of a stream. An action's other fields mean nothing, but a machine crash's seed and
    // a restore's instrument.
    struct EngineCommand {
        std::size_t instrument;
        randomized::Command generated;  // as the generator made it
        std::optional<randomized::Command> converted;  // what reaches the engine, if anything
        SymbolFault fault = SymbolFault::NONE;
        EngineAction action = EngineAction::COMMAND;
        std::uint64_t crashSeed = 0;
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
        std::size_t logFiles = 0;          // every shard's, the first included
        std::size_t crashes = 0;
        std::size_t lostToCrashes = 0;     // queued commands no recovery found
        std::uint64_t tailCutBytes = 0;    // what every recovery cut
        std::size_t restores = 0;          // RESTORE lines run
    };

    struct EngineRunResult {
        std::optional<EngineFailure> failure;
        EngineStats stats;

        bool passed() const { return !failure.has_value(); }
    };

    // Every queued command goes to its shard's log ring, as the matching thread sends it, and its
    // log writer writes on LOG lines and whenever that ring is full, on log files of 25 records
    // for the shard with the most symbols, so each run rolls across many. The housekeeping thread
    // is played whenever a writer waits for its next file. The run ends by stopping each log
    // writer, then replaying each shard's files in order, each starting where the last ended, per
    // instrument through ReplayEngine, which must give a book equal to the witness, and checking
    // each Shard, durable position included.
    //
    // A RESTORE line requires the shard's book's image to equal its witness's, restore to rebuild
    // it, and writeImage(restore(x)) == x, then puts the restored book in the shard, so restores
    // compound.
    //
    // A CRASH line first notes, per shard, the durable position its responses were released up to.
    // A machine crash then has every shard's log writer meet it in one more write.
    // The storage crashes, every shard, log writer, and the housekeeping are dropped, and the
    // start runs as the engine's does: the lock, surveyLog, the temporaries removed, and
    // recoverShard into new shards, whose log writers go on where each log ended. The model and
    // witness are rebuilt from the commands that survived, and every book must equal both. At
    // machine durability, and at process for an engine crash, each shard must keep every
    // command it had released. Every file is far below 1 MiB, so recovery cuts every hole a
    // crash leaves, and any stop fails the run.
    EngineRunResult runEngineSeed(const EngineSeedConfig& config);

    struct ShardLogRecords {
        std::vector<replay::ReplayRecord> records;  // from position 0, in order
        std::size_t files = 0;
    };

    // Every record of one shard's files in the storage, read back through CommandLogReader, never
    // the encoder, file by file in the order their names give. Throws std::runtime_error if a file
    // wasn't closed whole, doesn't read back, or doesn't start where the last one ended.
    ShardLogRecords readShardLog(const runtime::InMemoryLogStorage& storage, std::uint32_t shard);

}  // namespace order_book::engine_testing
