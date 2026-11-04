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

// Recovery from position 0 (spec #58's D2, ADR 0013): what a start does with the log an earlier
// run left, before any book is ready. Until checkpoints land every book replays its shard's whole
// log, no log file holding a record is deleted, and any placement change is refused while a log
// exists. Not public API. See docs/internals/runtime.md.
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

    // What the main thread found: each shard's log files, oldest first, and the files a crash
    // left being made, which held no record anyone was told of.
    struct LogSurvey {
        std::vector<std::vector<LogFileEntry>> shards;  // by shard, one per shard of the directory
        std::vector<std::string> temporaries;
    };

    // Recovery's start on the main thread, once the caller holds the storage's lock, before any
    // book exists: reads every log file's header, refuses a symbol off the list, then compares
    // each shard's newest header with the directory's placement. Throws RecoveryError, and
    // LogStorageError if a file can't be read. Changes nothing.
    LogSurvey surveyLog(LogStorage& storage, const InstrumentDirectory& directory);

    struct RecoveredShard {
        std::uint64_t replayed = 0;       // the log's end, where its next file starts
        std::uint64_t tailCutBytes = 0;   // written bytes a cut tail or a removed file dropped, zeros not
        std::size_t filesRemoved = 0;     // trailing files with no records
        std::vector<BookCapacity> warmed;  // by slot
    };

    // One shard's recovery, on its matching thread, into a shard just built at position 0 from the
    // same directory: removes trailing files with no records, replays the rest through
    // Shard::process, discarding events, runs checkInvariants on every book, warms each book to
    // the larger of what it declares and 1.5 times what it holds, each number apart, then cuts a
    // torn tail off the last file, or a hole valid records follow within tornWriteReach of its
    // end, and flushes it, removing it if nothing is left in it. Opening the next file is the log
    // writer's. A TORN_TAIL or CORRUPT_RECORD in an earlier file, one further back, a gap, any
    // other finding, or a broken book throws RecoveryError; a request in stopping, read between
    // records, throws RecoveryInterrupted.
    RecoveredShard recoverShard(LogStorage& storage, const InstrumentDirectory& directory, const std::vector<LogFileEntry>& files,
                                Shard& shard, const BookCapacity& defaults, const std::atomic<bool>& stopping);

}  // namespace order_book::runtime
