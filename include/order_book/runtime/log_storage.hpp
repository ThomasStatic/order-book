#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "order_book/runtime/durability.hpp"

// Every file the log touches goes through LogStorage (ADR 0013), so the log writer is a plain
// object over it, and a test can hold its files in memory. Not public API. See
// docs/internals/runtime.md.
namespace order_book::runtime {

    // A storage call that failed. A log write or flush that fails aborts the engine, since no
    // retry can be trusted; one at start refuses it.
    class LogStorageError : public std::runtime_error {
    public:
        // LOCKED: someone else holds the directory's lock.
        enum class Kind { EXISTS, FAILED, LOCKED };

        LogStorageError(Kind kind, const std::string& what) : std::runtime_error(what), errorKind(kind) {}
        Kind kind() const noexcept { return errorKind; }

    private:
        Kind errorKind;
    };

    // One open log file, used by one thread. Every failure throws LogStorageError.
    class LogFile {
    public:
        virtual ~LogFile() = default;

        // Copies up to out.size() bytes from offset, and returns how many there were: fewer only
        // at the file's end. Past the written part, up to the size it was extended to, it reads
        // zeros.
        virtual std::size_t read(std::uint64_t offset, std::span<std::byte> out) = 0;
        virtual std::uint64_t size() = 0;

        // Returns once the bytes hold at the file's durability level.
        virtual void write(std::uint64_t offset, std::span<const std::byte> bytes) = 0;
        // Everything written so far, onto the drive.
        virtual void flush() = 0;
        // Makes the file size bytes long without writing them, so that what lies past the written
        // part reads as zeros, which TORN_TAIL needs. Never shortens it.
        virtual void extend(std::uint64_t size) = 0;
        // Drops everything from size on: a clean stop cuts the zero tail an extended file has.
        virtual void cut(std::uint64_t size) = 0;
        virtual void close() = 0;
        // Gives the file a new name in its directory, never over an existing file, which throws
        // with Kind::EXISTS, and returns once the new name survives a machine crash. Its
        // messages name it by its new name from then on.
        virtual void rename(const std::string& name) = 0;
        // The same, but over a file of that name if there is one, in one step: a machine crash
        // leaves the old file under the name or this one, never neither. Only a placement change
        // uses it, to rewrap a checkpoint whose name stays the same.
        virtual void replace(const std::string& name) = 0;
    };

    // A directory's lock, held until this is destroyed, or the process ends, however it ends.
    class StorageLock {
    public:
        virtual ~StorageLock() = default;
    };

    class LogStorage {
    public:
        virtual ~LogStorage() = default;

        // The lock on the directory, so a second engine or an offline command on it is refused,
        // with Kind::LOCKED, while this one holds it.
        virtual std::unique_ptr<StorageLock> lock() = 0;
        // Every file's name, sorted.
        virtual std::vector<std::string> names() const = 0;
        // An existing file, to read, and at recovery to cut and flush. A file that isn't there
        // is Kind::FAILED.
        virtual std::unique_ptr<LogFile> open(const std::string& name) = 0;

        // A new file, never an existing one, which throws with Kind::EXISTS: old files are never
        // reused, since their stale records still chain. Its name may not survive a machine
        // crash until it is renamed.
        virtual std::unique_ptr<LogFile> create(const std::string& name, Durability durability) = 0;
        virtual void remove(const std::string& name) = 0;
        // Where a file is, for a message.
        virtual std::string describe(const std::string& name) const = 0;
    };

    // log-<shard, 4 digits>-<first position, 20 digits>.oblog
    std::string logFileName(std::uint32_t shard, std::uint64_t firstPosition);

    // What a log file's name says, the name plus .tmp of one being made included.
    struct LogFileNameParts {
        std::uint32_t shard = 0;
        std::uint64_t firstPosition = 0;
        bool temporary = false;
    };

    // What logFileName gives, or that plus .tmp, read back; nothing for any other name.
    std::optional<LogFileNameParts> parseLogFileName(std::string_view name);
    // log- and .oblog or .oblog.tmp, whether or not an engine names a file so.
    bool namedLikeALogFile(std::string_view name);

    // The real files, in one directory, which must exist. Its lock is engine.lock there, empty
    // and never deleted, under an OS file lock (LockFileEx, flock), so a crash releases it.
    class FileLogStorage final : public LogStorage {
    public:
        static constexpr const char* lockFileName = "engine.lock";

        explicit FileLogStorage(std::filesystem::path directory);

        std::unique_ptr<StorageLock> lock() override;
        std::vector<std::string> names() const override;
        std::unique_ptr<LogFile> open(const std::string& name) override;
        std::unique_ptr<LogFile> create(const std::string& name, Durability durability) override;
        void remove(const std::string& name) override;
        std::string describe(const std::string& name) const override;

    private:
        std::filesystem::path directory;
    };

    // Files held in memory, for tests, under one lock, so a test's thread may read them while a
    // log writer's writes. It can fail a chosen create, write, or flush, as a full or broken
    // drive would. An extended file costs nothing past what was written.
    //
    // It models both crashes (spec #58's seam 5). Each file has what the OS holds, which every
    // read sees, and what the drive holds: a write at machine durability reaches both, one at
    // process durability only the OS, until a flush takes the whole file, its size included, to
    // the drive. A directory reaches the drive when a rename flushes it, as makeLogFile's does:
    // until then a created file's name and a remove are the OS's alone. An engine crash keeps
    // what the OS holds. A machine crash keeps what the drive holds, and of each page the OS
    // holds otherwise, a seeded choice of all of it, none, or its first sectors, which is a torn
    // write; a size the OS holds otherwise, a cut's or an extend's, lands whole or not at all; and
    // a removed file the directory still names comes back or not.
    class InMemoryLogStorage final : public LogStorage {
    public:
        static constexpr std::size_t pageBytes = 4096;
        static constexpr std::size_t sectorBytes = 512;

        struct File {
            std::vector<std::byte> bytes;  // up to the end of the last write; zeros follow, to size
            std::uint64_t size = 0;
            std::vector<std::byte> onDrive;  // the same, as the drive holds them
            std::uint64_t sizeOnDrive = 0;
            bool nameOnDrive = false;
            Durability durability = Durability::MACHINE;
            std::size_t writes = 0;
            std::size_t flushes = 0;
            std::size_t extends = 0;
            std::size_t renames = 0;
            std::size_t cuts = 0;
            bool closed = false;  // since its last write, extend, or cut, by a handle or a crash
            // Removed by a replace that never reached the drive, so a machine crash brings it
            // back for certain: the drive's directory never left it.
            bool replacedOffDrive = false;
        };

        // Every file, by name, as it stands, and the files removed since the directory was last
        // flushed, which a machine crash may bring back.
        struct Contents {
            std::map<std::string, File> files;
            std::map<std::string, File> removed;
        };

        enum class Crash { ENGINE, MACHINE };

        InMemoryLogStorage() = default;
        // Holding just what one held, as a restart on its drive finds it.
        explicit InMemoryLogStorage(const Contents& from);

        std::unique_ptr<StorageLock> lock() override;
        std::vector<std::string> names() const override;
        std::unique_ptr<LogFile> open(const std::string& name) override;
        std::unique_ptr<LogFile> create(const std::string& name, Durability durability) override;
        void remove(const std::string& name) override;
        std::string describe(const std::string& name) const override;

        // A copy, or nothing for a file that isn't there.
        std::optional<File> file(const std::string& name) const;
        // The bytes written from offset on, which is how a test follows a file as it grows,
        // leaving out an extended file's zeros; empty for a file that isn't there or holds no
        // more than that.
        std::vector<std::byte> read(const std::string& name, std::size_t offset) const;
        // From now on, the next create or write after this many succeed throws. So does every
        // flush once failFlushes is set.
        void failCreateAfter(std::size_t creates);
        void failWriteAfter(std::size_t writes);
        void failFlushes();

        Contents contents() const;
        // Called after each create, write, extend, cut, flush, rename, and remove, with what the
        // storage then holds, on the thread that made the change, outside the storage's lock.
        void watch(std::function<void(const Contents&)> afterEachChange);
        // From now until a crash, nothing reaches the drive: the writes a machine crash meets on
        // their way, whatever their durability, and anything after them.
        void interruptWrites();
        // The crash, in place: every open file and the lock belong to the engine that died, so
        // they reach nothing from now on, and the storage holds what a restart finds.
        void crash(Crash kind, std::uint64_t seed);

    private:
        class OpenFile;
        class HeldLock;

        Contents contentsHeld() const;
        // After a change, with the guard taken: tells the watcher, which runs once it is let go.
        void changed(std::unique_lock<std::mutex>& held);

        mutable std::mutex guard;
        // Shared with each open file, so a remove leaves it writing where it was.
        std::map<std::string, std::shared_ptr<File>> files;
        std::map<std::string, File> removed;  // since the directory was last flushed
        std::optional<std::size_t> createsBeforeFailure;
        std::optional<std::size_t> writesBeforeFailure;
        bool flushesFail = false;
        bool locked = false;
        std::uint64_t lockGeneration = 0;  // a crash moves it on, so a dead engine's lock lets go of nothing
        bool interrupted = false;
        std::function<void(const Contents&)> watcher;
    };

}  // namespace order_book::runtime
