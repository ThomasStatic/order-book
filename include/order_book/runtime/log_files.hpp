#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "order_book/command_log.hpp"
#include "order_book/runtime/durability.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/log_storage.hpp"

// A shard's log goes on files of a fixed size, each made ahead by the engine's housekeeping
// thread, so a log writer never waits for one unless the drive is slow. Not public API. See
// docs/internals/runtime.md.
namespace order_book::runtime {

    // What stays the same from one of a shard's log files to the next.
    struct LogFileShape {
        std::uint32_t shard = 0;
        replay::LogHeader header;  // the placement; each file sets its own first position
        Durability durability = Durability::MACHINE;
        std::uint64_t headerBytes = 0;
        std::uint64_t records = 0;  // per file
    };

    // A shard's files of at most fileBytes each: its header, then as many whole records as fit,
    // so a full file ends on a record and never reads as a torn tail. Throws
    // std::invalid_argument when the header and one record don't fit.
    LogFileShape logFileShape(const InstrumentDirectory& directory, std::uint32_t shard, Durability durability, std::uint64_t fileBytes);

    // The smallest file size every shard's log can use: its largest header and one record.
    std::uint64_t leastLogFileBytes(const InstrumentDirectory& directory);

    // One log file as made: its header written, extended to hold its records, and flushed.
    struct MadeLogFile {
        std::string name;
        std::unique_ptr<LogFile> file;
        std::uint64_t firstPosition = 0;
        std::uint64_t headerBytes = 0;  // where the first record goes
        std::uint64_t records = 0;      // room for
        std::uint32_t chainSeed = 0;
    };

    // Makes the file under its name plus .tmp, writes its header, extends it, which allocates
    // without writing, so a full drive fails here, flushes it, and renames it, so a log file
    // anyone can see has its whole header and its name survives a machine crash. A failure throws
    // LogStorageError and leaves neither name behind, unless the name was already taken, which
    // is Kind::EXISTS.
    MadeLogFile makeLogFile(LogStorage& storage, const LogFileShape& shape, std::uint64_t firstPosition);

    // The engine's one housekeeping thread, as a plain object, so a test can step it on one
    // thread and the thread only calls run. For each shard whose log has started it keeps one
    // next file made, which the shard's log writer takes when its file is full; taking it asks
    // for the one after. It makes them all, one at a time, off every matching thread. A next
    // file that can't be made aborts at once, while the current file still has room, since the
    // log could never go on. The storage must outlive it.
    class Housekeeping {
    public:
        Housekeeping(LogStorage& storage, const InstrumentDirectory& directory, Durability durability, std::uint64_t fileBytes);

        Housekeeping(const Housekeeping&) = delete;
        Housekeeping& operator=(const Housekeeping&) = delete;

        const LogFileShape& shape(std::uint32_t shard) const { return shapes.at(shard); }

        // A shard's first file, made on the calling thread, at start: a failure throws
        // LogStorageError, which refuses the start. From then its next file is wanted.
        MadeLogFile first(std::uint32_t shard, std::uint64_t firstPosition);

        // The log writer's: the shard's next file, if it is made yet.
        std::optional<MadeLogFile> takeNext(std::uint32_t shard);
        // The log writer's: waits until the shard's next file is made, or housekeeping stops,
        // and says whether it is made.
        bool waitForNext(std::uint32_t shard);

        // Makes every next file wanted, then deletes what retention allows, and returns how many
        // files it made and deleted. A file that can't be made aborts, naming it; one that can't
        // be deleted is printed and tried again once the shard's floor next rises.
        std::size_t pass();

        // Retention (spec #58's D5): every closed file of the shard ending at or before floor,
        // which no book's kept checkpoint needs, goes on a later pass. A file is closed once the
        // next one starts at or before floor, so the writer's file, the one made ahead, and the
        // shard's last are never deleted. A floor below one given before changes nothing.
        void retain(std::uint32_t shard, std::uint64_t floor);
        // --keep-log: delete nothing, whatever retain says.
        void keepEveryFile() noexcept;
        std::uint64_t filesDeleted() const noexcept { return deleted.load(std::memory_order_relaxed); }
        // Passes until stop, sleeping while no file is wanted.
        void run();
        void stop();

        // Once run has returned and no log writer still takes: closes and removes each file made
        // that no writer took, as a clean stop does. Throws LogStorageError if one can't be
        // removed, after trying the rest.
        void removeUnused();

    private:
        struct ShardFiles {
            bool started = false;
            std::uint64_t nextPosition = 0;  // the first position of the next file to make
            std::optional<MadeLogFile> next;
            std::uint64_t floor = 0;   // retention's: files ending at or before it may go
            std::uint64_t pruned = 0;  // the floor every file below which is gone
        };

        bool wanted(const ShardFiles& shard) const noexcept { return shard.started && !shard.next; }
        bool pruneWanted(const ShardFiles& shard) const noexcept { return !keepLog && shard.floor > shard.pruned; }
        // Deletes the shard's closed files ending at or before floor; whether every one went.
        bool prune(std::uint32_t shard, std::uint64_t floor);

        LogStorage& logStorage;
        std::vector<LogFileShape> shapes;  // by shard
        std::mutex lock;
        std::condition_variable changed;
        std::vector<ShardFiles> shards;  // by shard, under the lock
        bool stopping = false;           // under the lock
        bool keepLog = false;            // under the lock
        std::atomic<std::uint64_t> deleted{0};
    };

}  // namespace order_book::runtime
