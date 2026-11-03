#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
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
        enum class Kind { EXISTS, FAILED };

        LogStorageError(Kind kind, const std::string& what) : std::runtime_error(what), errorKind(kind) {}
        Kind kind() const noexcept { return errorKind; }

    private:
        Kind errorKind;
    };

    // One open log file, written by one thread. Every failure throws LogStorageError.
    class LogFile {
    public:
        virtual ~LogFile() = default;

        // Returns once the bytes hold at the file's durability level.
        virtual void write(std::uint64_t offset, std::span<const std::byte> bytes) = 0;
        // Everything written so far, onto the drive.
        virtual void flush() = 0;
        // Drops everything from size on: a clean stop cuts the zero tail an extended file has.
        virtual void cut(std::uint64_t size) = 0;
        virtual void close() = 0;
    };

    class LogStorage {
    public:
        virtual ~LogStorage() = default;

        // A new file, never an existing one, which throws with Kind::EXISTS: old files are never
        // reused, since their stale records still chain.
        virtual std::unique_ptr<LogFile> create(const std::string& name, Durability durability) = 0;
        virtual void remove(const std::string& name) = 0;
        // Where a file is, for a message.
        virtual std::string describe(const std::string& name) const = 0;
    };

    // log-<shard, 4 digits>-<first position, 20 digits>.oblog
    std::string logFileName(std::uint32_t shard, std::uint64_t firstPosition);

    // The real files, in one directory, which must exist.
    class FileLogStorage final : public LogStorage {
    public:
        explicit FileLogStorage(std::filesystem::path directory);

        std::unique_ptr<LogFile> create(const std::string& name, Durability durability) override;
        void remove(const std::string& name) override;
        std::string describe(const std::string& name) const override;

    private:
        std::filesystem::path directory;
    };

    // Files held in memory, for tests, under one lock, so a test's thread may read them while a
    // log writer's writes. It can fail a chosen write or flush, as a full or broken drive would.
    class InMemoryLogStorage final : public LogStorage {
    public:
        struct File {
            std::vector<std::byte> bytes;
            Durability durability = Durability::MACHINE;
            std::size_t writes = 0;
            std::size_t flushes = 0;
            std::size_t cuts = 0;
            bool closed = false;
        };

        std::unique_ptr<LogFile> create(const std::string& name, Durability durability) override;
        void remove(const std::string& name) override;
        std::string describe(const std::string& name) const override;

        // A copy, or nothing for a file that isn't there.
        std::optional<File> file(const std::string& name) const;
        // A file's bytes from offset on, which is how a test follows a file as it grows; empty
        // for a file that isn't there or is no longer than that.
        std::vector<std::byte> read(const std::string& name, std::size_t offset) const;
        std::vector<std::string> names() const;
        // The next write, from now on, after this many succeed, throws. So does every flush once
        // failFlushes is set.
        void failWriteAfter(std::size_t writes);
        void failFlushes();

    private:
        class OpenFile;

        mutable std::mutex lock;
        std::map<std::string, File> files;
        std::optional<std::size_t> writesBeforeFailure;
        bool flushesFail = false;
    };

}  // namespace order_book::runtime
