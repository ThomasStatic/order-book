#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "order_book/runtime/checkpoint.hpp"
#include "order_book/runtime/closed_log.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/symbol.hpp"

// Rolling checkpoints forward from the log (ADR 0014, spec #58's D4b): a shard's books are made
// from their last checkpoints and the closed log files after them, away from the matching
// thread, which never reads or pauses for one. Plain objects over LogStorage, so a worker thread,
// the engine harness, and tests all run the same pass. Not public API. See
// docs/internals/runtime.md.
namespace order_book::runtime {

    // ckpt-<symbol>-<position, 20 digits>.obckpt: the prefix keeps a symbol such as AUX, CON, or NUL,
    // which Windows refuses as a file name, from being one alone.
    std::string checkpointFileName(const Symbol& symbol, std::uint64_t position);

    struct CheckpointFileNameParts {
        Symbol symbol;
        std::uint64_t position;
        bool temporary;  // a .tmp being published, which a crash may leave
    };

    // What a checkpoint file's name says, or nothing for a name no engine makes.
    std::optional<CheckpointFileNameParts> parseCheckpointFileName(std::string_view name);

    // The image proof: restore the image, run checkInvariants, and write it again, which must give
    // the same bytes. What failed, or nothing.
    std::optional<std::string> proveImage(std::span<const std::byte> image);

    // proveImage, aborting on a failure with "order book runtime: roll forward: <what>: the image
    // proof failed: <why>", since a book rolled forward from the log that does not prove is a bug
    // the live book shares.
    void requireProvedImage(std::span<const std::byte> image, std::string_view what);

    // Publishes one checkpoint: writes <name>.tmp, reads it back and decodes it, which must give
    // the header and image written, flushes it, and renames it, which flushes the directory. A
    // storage failure, or a read-back that differs, throws LogStorageError, removing the .tmp and
    // leaving every checkpoint already published in place.
    void publishCheckpoint(LogStorage& storage, const CheckpointHeader& header, std::span<const std::byte> image);

    // A new instrument's checkpoint (spec #58): an empty book at the shard's recovered log end,
    // published after the tail is cut and before the next file opens, so "never checkpointed" is
    // not a case. Throws as publishCheckpoint does.
    void publishEmptyCheckpoint(LogStorage& storage, const Symbol& symbol, std::uint32_t shardCount, std::uint32_t shard,
                                std::uint64_t position);

    // What a shard's roll forward has done since it was made.
    struct RollCounts {
        std::uint64_t passes = 0;          // closed files rolled, each publishing its books
        std::uint64_t published = 0;       // checkpoints published
        std::uint64_t writesFailed = 0;    // checkpoints whose publishing failed: the book stays behind
        std::uint64_t passesLost = 0;      // passes a bad_alloc discarded, to be made again
    };

    // One shard's roll forward. It reads the store's checkpoint names once, when made, and keeps
    // each book's two newest positions from then on, which is all it publishes or removes: one
    // instance per shard per store, on one thread at a time. When made, as at a start, it removes
    // its books' leftover .tmp files, any checkpoint that does not decode as its name says, and
    // any older than a book's two. A storage failure there throws LogStorageError; during a roll
    // it is reported and the next roll tries again.
    //
    // A pass rolls one closed file: the books standing at its first position are restored, if
    // they have commands in it, on first use; every command is replayed on them; each one rolled
    // is proved; and every one is published at the file's end, a book with no command getting a
    // new wrapper around the same image. A failed proof aborts, since the worker's book and the
    // live one ran the same commands through the same code. Until checkpoints are recovered from
    // (D5), a book with no checkpoint is an empty book at the first file whose header lists it.
    class ShardRoller {
    public:
        // Called with each failure as it happens: a checkpoint not published, or a pass lost.
        using Report = std::function<void(const std::string&)>;

        ShardRoller(LogStorage& storage, const InstrumentDirectory& directory, std::uint32_t shard, Report report);

        // Rolls every closed file a book of this shard needs, oldest first, each at most once,
        // where a file is closed once the next one starts at or before closedEnd. A book whose
        // checkpoint failed to publish waits for the next call. A bad_alloc discards that pass's
        // books, is counted, and ends the call; the next call makes the pass again from the last
        // good checkpoints. With `stopping`, it stops between passes once that is set. Returns the
        // passes made.
        std::size_t roll(std::uint64_t closedEnd, const std::atomic<bool>* stopping = nullptr);

        // Closed files, up to the last roll's closedEnd, that some book still needs.
        std::size_t filesBehind() const noexcept { return behind; }

        const RollCounts& counts() const noexcept { return totals; }

        // A book's newest checkpoint position, or nothing if it has none yet.
        std::optional<std::uint64_t> position(std::uint32_t slot) const;

    private:
        struct Kept {
            std::optional<std::uint64_t> newest;
            std::optional<std::uint64_t> older;
            std::optional<std::uint64_t> superseded;  // published over, not yet removed
        };

        struct LogFileInfo {
            std::string name;
            std::uint64_t firstPosition = 0;
            std::uint64_t endPosition = 0;  // the next file's first
            std::uint64_t headerBytes = 0;
            std::uint32_t shardCount = 0;
            std::vector<std::uint32_t> slots;  // the books its header lists, by slot
        };

        void readLogFiles(std::uint64_t closedEnd);
        void pass(const LogFileInfo& file, std::uint64_t end, const std::vector<bool>& needs);
        // Where a book stands: its newest checkpoint, or the first file that lists it.
        std::optional<std::uint64_t> standing(std::uint32_t slot) const;
        bool standsAt(std::uint32_t slot, const LogFileInfo& file) const;
        // The book's newest checkpoint no longer reads: the older becomes its newest, and the
        // pass is abandoned, to be made again from there.
        [[noreturn]] void fallBack(std::uint32_t slot, const std::string& why);
        // A report that never throws, for what a catch says.
        void say(const std::string& lead, const char* what) noexcept;
        void removeSuperseded(std::uint32_t slot);

        LogStorage& storage;
        const InstrumentDirectory& directory;
        std::uint32_t shard;
        Report report;
        std::vector<Kept> kept;              // by slot
        // By slot, where a book with no checkpoint stands: the first file whose header lists it.
        std::vector<std::optional<std::uint64_t>> firstListing;
        std::vector<LogFileInfo> files;      // this shard's closed ones, by first position
        std::size_t behind = 0;
        RollCounts totals;
    };

    // A roll-forward worker (--roll-workers): its shards' rollers, made on the thread that starts
    // the engine, rolled on the worker's own thread whenever a log writer closes a file. Nothing
    // here touches a live book.
    class RollWorker {
    public:
        // Makes a ShardRoller per shard, so a leftover .tmp goes and each book's checkpoints are
        // read before the worker runs. A storage failure throws LogStorageError.
        RollWorker(LogStorage& storage, const InstrumentDirectory& directory, const ClosedLog& closed, std::vector<std::uint32_t> shards,
                   ShardRoller::Report report);

        // One roll of each of its shards, at the end its log writer last closed. Returns the passes.
        std::size_t pass();

        // Passes until the ClosedLog stops: once, then each time a file closes.
        void run();

        const std::vector<std::uint32_t>& shards() const noexcept { return mine; }
        const ShardRoller& roller(std::size_t index) const { return *rollers.at(index); }

    private:
        const ClosedLog& closed;
        std::vector<std::uint32_t> mine;
        ShardRoller::Report report;
        std::vector<std::unique_ptr<ShardRoller>> rollers;  // by index in mine
    };

}  // namespace order_book::runtime
