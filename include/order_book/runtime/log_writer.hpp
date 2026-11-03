#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "order_book/command.hpp"
#include "order_book/command_log.hpp"
#include "order_book/runtime/doorbell.hpp"
#include "order_book/runtime/durable_position.hpp"
#include "order_book/runtime/log_files.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/ring.hpp"

// The log writer and the durable position (ADR 0013): a command matches at once, and only its
// response waits until its place in the log is durable. Not public API. See
// docs/internals/runtime.md.
namespace order_book::runtime {

    // One element of a shard's log ring, from its matching thread to its log writer. A STOP is
    // the matching thread's last, once it has stopped: the writer then finishes the file.
    struct LogEntry {
        replay::Command command;
        std::uint32_t slot;
        ControlKind control;

        static constexpr LogEntry stop() noexcept { return LogEntry{replay::CancelCommand{0}, 0, ControlKind::STOP}; }
    };
    static_assert(std::is_trivially_copyable_v<LogEntry>, "a log entry crosses threads by copy");

    // One shard's log writer, as a plain object, so a test can step it on one thread and the
    // thread itself only runs it. It alone encodes, checksums, and writes. Each pass is one write
    // of whatever has queued, with no batching timer, capped at 1 MiB and at the room left in
    // its file; then it publishes the durable position. A file it fills it flushes and closes at
    // once, and the next record goes on the next file, which the housekeeping thread made ahead.
    // The housekeeping, ring, and durable position must outlive it.
    class LogWriter {
    public:
        static constexpr std::size_t mostBytesPerWrite = std::size_t{1} << 20;
        static constexpr std::size_t mostRecordsPerWrite = mostBytesPerWrite / replay::logRecordBytes;

        // Makes the shard's first file, named for its first position, header and placement
        // included. A storage failure throws LogStorageError, which refuses a start.
        LogWriter(Housekeeping& housekeeping, std::uint32_t shard, Ring<LogEntry>& ring, DurablePosition& durable,
                  std::uint64_t firstPosition = 0);

        LogWriter(const LogWriter&) = delete;
        LogWriter& operator=(const LogWriter&) = delete;

        // One write of up to mostRecordsPerWrite records from the ring, never past the end of
        // its file, then the durable position moves past them. A file it fills, it flushes and
        // closes. A record with no room left waits for the next file: the pass then writes
        // nothing, and waitingForFile says so. After the matching thread's STOP it writes the
        // rest, cuts the file to its last record, flushes, and closes it. Returns the records
        // written. A failed write, flush, cut, or close aborts the process, naming the shard and
        // file: a retry can't be trusted, and no response has gone out for those commands.
        std::size_t pass();

        // Passes until the STOP, spinning for the spin window on an empty ring, then sleeping
        // until the matching thread rings, or until the next file is made.
        void run(Doorbell& doorbell, std::chrono::nanoseconds spinWindow);

        bool stopped() const noexcept { return finished; }
        bool waitingForFile() const noexcept { return waiting; }
        // The file being written, or the last one.
        const std::string& fileName() const noexcept { return name; }
        // The position the next record will have.
        std::uint64_t nextPosition() const noexcept { return next; }

    private:
        [[noreturn]] void abortOnFailure(const char* what) const;
        void begin(MadeLogFile&& made);

        Housekeeping& housekeeping;
        Ring<LogEntry>& ring;
        DurablePosition& durable;
        std::uint32_t shard;
        std::vector<Symbol> symbols;  // by slot
        std::string name;
        std::unique_ptr<LogFile> file;  // none once a full file is closed, until the next
        std::uint64_t room = 0;         // records the file has left
        std::vector<std::byte> buffer;
        std::uint64_t offset = 0;  // where the next write goes
        std::uint64_t next;
        std::uint32_t checksum = 0;
        bool waiting = false;
        bool finished = false;
    };

}  // namespace order_book::runtime
