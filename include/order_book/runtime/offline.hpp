#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/roll_forward.hpp"
#include "order_book/symbol.hpp"

// The offline commands' work (spec #58's D7): verify, checkpoint roll, and cut-log, over a store
// no engine is using. Each takes its directories' locks, the checkpoints' only where they are
// another store, and returns what it found, printing nothing; order_book_cli prints it. Not
// public API. See docs/internals/runtime.md.
namespace order_book::runtime {

    // A store's placement as its own files give it, since the offline commands take no instrument
    // list: the shard count shard 0's newest log header names, each symbol pinned to the current
    // shard whose newest header lists it, and a symbol only a checkpoint names pinned to the
    // shard its newest checkpoint says. Throws RecoveryError (START_REFUSED) for a store with no
    // log, and LogStorageError if a file can't be read.
    InstrumentDirectory directoryOfStore(LogStorage& logs, LogStorage& checkpoints);

    struct VerifiedBook {
        Symbol symbol;
        std::uint64_t checkpoint = 0;  // the position it restores from, or where a new one starts
        bool fellBack = false;         // its newest checkpoint didn't restore
        bool isNew = false;            // no checkpoint: an empty book a start gives one
    };

    // What a start would make of one shard: where its replay starts and its log ends, and what it
    // would drop, which verify only counts.
    struct VerifiedShard {
        std::uint32_t shard = 0;
        std::uint64_t start = 0;
        std::uint64_t end = 0;
        std::size_t files = 0;
        std::uint64_t tailCutBytes = 0;
        std::size_t filesDropped = 0;
        std::vector<VerifiedBook> books;
    };

    enum class VerifyOutcome {
        OK,       // exit 0: a start would pass, a torn tail included
        STOPPED,  // exit 1: a start would stop or be refused, or the store is in use
        BROKEN,   // exit 3: a book broke, naming the shard, position, file, symbol, and command
    };

    struct StoreVerification {
        std::vector<VerifiedShard> shards;  // every shard, or those before the failure
        VerifyOutcome outcome = VerifyOutcome::OK;
        std::string failure;
    };

    // verify: recovery's checks and replay, read-only, every book checked once replayed or, with
    // perCommand, after each command it runs. check is a test's stand-in for a poison command.
    StoreVerification verifyStore(LogStorage& logs, LogStorage& checkpoints, bool perCommand, const BookCheck& check = {});

    // A shard's lines: "shard 0: start=24 end=30 files=2 tail-cut-bytes=0 files-dropped=0", then
    // "book AAPL shard=0 checkpoint=24" for each book, with " fell-back" or " new" after it.
    std::string formatVerifiedShard(const VerifiedShard& shard);

    enum class RollOutcome {
        OK,           // exit 0
        FAILED,       // exit 1: a checkpoint was not published, or the store is in use
        PROOF_FAILED, // exit 3
    };

    struct StoreRoll {
        std::size_t files = 0;      // closed files rolled
        std::uint64_t books = 0;    // checkpoints published
        std::vector<std::string> reported;  // what the rollers reported as it happened
        RollOutcome outcome = RollOutcome::OK;
        std::string failure;
    };

    // checkpoint roll: every shard's roll forward over its closed files, as a worker makes it, a
    // shard's last file being closed only once a later one starts; a failed proof is
    // PROOF_FAILED, not an abort. check is part of the proof, for tests.
    StoreRoll rollStore(LogStorage& logs, LogStorage& checkpoints, const BookCheck& check = {});

    struct LogCut {
        bool cut = false;
        std::uint64_t records = 0;  // moved
        std::string file;           // the cut- file holding them
        std::string failure;        // exit 1 when not cut
    };

    // cut-<shard, 4 digits>-<position, 20 digits>.oblog, #34's name for cut records.
    std::string cutFileName(std::uint32_t shard, std::uint64_t position);

    // cut-log: the record at position and every one after it, moved from the shard's log to a
    // cut- file, every byte of each kept: a valid log under a header like the one they were
    // written under, its chain sealed anew as far as theirs held, and copied as they are past
    // that, as a damaged record and those after it are. The file is complete before the log is
    // touched; then the files after the record's are removed, newest first, and its own is cut
    // at it. Run again after a crash, it finishes the cut when the log holds a prefix of what the
    // cut file does. Refused if a checkpoint of the shard stands past position.
    LogCut cutLog(LogStorage& logs, LogStorage& checkpoints, std::uint32_t shard, std::uint64_t position);

}  // namespace order_book::runtime
