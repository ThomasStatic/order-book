#pragma once

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "order_book/order_book.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/shard.hpp"

// Recovery (spec #58's D2 and D5, ADRs 0013 and 0014): what a start does with the log and the
// checkpoints an earlier run left, before any book is ready. Each book restores from its newest
// good checkpoint, or the older one, and its shard replays the log from the oldest book's
// position, each record older than its own book's skipped. Until D6, any placement change is
// refused while a log exists. Not public API. See docs/internals/runtime.md.
namespace order_book::runtime {

    // A start the log refuses, with what the engine prints after its prefix. START_REFUSED is
    // exit 1 ("start refused: <what>"), PLACEMENT_CHANGED exit 4 ("start refused: placement
    // changed: <what>"), and RECOVERY_STOPPED exit 5 ("recovery stopped: <what>"), whose what
    // names the shard, position, file, symbol, and command, as far as each is known.
    class RecoveryError : public std::runtime_error {
    public:
        enum class Kind { START_REFUSED, PLACEMENT_CHANGED, RECOVERY_STOPPED };

        RecoveryError(Kind kind, const std::string& what) : std::runtime_error(what), errorKind(kind) {}
        Kind kind() const noexcept { return errorKind; }

    private:
        Kind errorKind;
    };

    // How far back from the end of what the last file holds a hole that valid records follow is
    // a write torn as a machine crash met it, cut at the hole: one write's most. Further back,
    // no crash explains it, and recovery stops.
    inline constexpr std::uint64_t tornWriteReach = std::uint64_t{1} << 20;

    // A stop requested while a shard replays, which ends the start: nothing is harmed.
    struct RecoveryInterrupted {};

    struct LogFileEntry {
        std::string name;
        std::uint64_t firstPosition = 0;
        bool operator==(const LogFileEntry&) const = default;
    };

    // What the main thread found: each shard's log files, oldest first, each book's checkpoints,
    // and the files a crash left being made, which held nothing anyone was told of.
    struct LogSurvey {
        std::vector<std::vector<LogFileEntry>> shards;  // by shard, one per shard of the directory
        std::vector<std::string> temporaries;            // log files being made
        // Every symbol some log file's header names, sorted.
        std::vector<Symbol> named;
        // By shard, then by slot: the positions of the book's checkpoints, newest first. Empty for
        // a new instrument.
        std::vector<std::vector<std::vector<std::uint64_t>>> checkpoints;
        // By shard: the new instruments' slots, named in no log header and with no checkpoint,
        // whose empty-book checkpoints the log writer publishes where the log ends.
        std::vector<std::vector<std::uint32_t>> newBooks;
        std::vector<std::string> checkpointTemporaries;  // checkpoints being published
    };

    // Recovery's start on the main thread, once the caller holds both stores' locks, before any
    // book exists: reads every log file's header and every checkpoint's name; refuses a symbol
    // off the list in either; stops on a lost checkpoint, a listed instrument a log header names
    // that no checkpoint holds; then compares each shard's newest header with the directory's
    // placement. Throws RecoveryError, and LogStorageError if a file can't be read. Changes
    // nothing. The checkpoint store may be the log's.
    LogSurvey surveyLog(LogStorage& logs, LogStorage& checkpoints, const InstrumentDirectory& directory);
    inline LogSurvey surveyLog(LogStorage& storage, const InstrumentDirectory& directory) {
        return surveyLog(storage, storage, directory);
    }

    struct RecoveredShard {
        std::uint64_t replayed = 0;       // the log's end, where its next file starts
        std::uint64_t tailCutBytes = 0;   // written bytes a cut tail or a removed file dropped, zeros not
        std::size_t filesRemoved = 0;     // trailing files with no records
        std::vector<BookCapacity> warmed;  // by slot
        std::uint64_t start = 0;           // where the replay began: the oldest book's position
        // By slot: the position of the checkpoint each book restored from, the start for a new one.
        std::vector<std::uint64_t> restoredFrom;
        std::size_t fellBack = 0;          // books whose newest checkpoint didn't restore
    };

    // One shard's recovery, on its matching thread, into a shard just built from the same
    // directory, with no command taken: restores each book from the first of its checkpoints
    // (newest first) that decodes, names it, stands under this placement, and restores; removes
    // trailing files with no records; starts the shard at the oldest book's position and replays
    // the log from there through Shard::process, discarding events, skipping each record older
    // than its own book's position; runs checkInvariants on every book; warms each book to the
    // larger of what it declares and 1.5 times what it holds, each number apart; then cuts a torn
    // tail off the last file, or a hole valid records follow within tornWriteReach of its end, and
    // flushes it, removing it if nothing is left in it. Opening the next file is the log writer's.
    // A book none of whose checkpoints restores, a log that starts after the oldest book (an
    // empty trailing file included), a checkpoint past the log's end (by name, one that didn't
    // decode included), a TORN_TAIL or CORRUPT_RECORD in an earlier file or further
    // back, a gap, any other finding, or a broken book throws RecoveryError; a checkpoint under
    // another placement is PLACEMENT_CHANGED. A request in stopping, read between records, throws
    // RecoveryInterrupted.
    RecoveredShard recoverShard(LogStorage& logs, LogStorage& checkpoints, const InstrumentDirectory& directory,
                                const std::vector<LogFileEntry>& files, const std::vector<std::vector<std::uint64_t>>& bookCheckpoints,
                                Shard& shard, const BookCapacity& defaults, const std::atomic<bool>& stopping);

}  // namespace order_book::runtime
