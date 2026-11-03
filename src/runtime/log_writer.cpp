#include "order_book/runtime/log_writer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <span>
#include <thread>

namespace order_book::runtime {

    LogWriter::LogWriter(Housekeeping& maker, std::uint32_t shardNumber, Ring<LogEntry>& logRing, DurablePosition& durablePosition,
                         std::uint64_t firstPosition)
        : housekeeping(maker),
          ring(logRing),
          durable(durablePosition),
          shard(shardNumber),
          symbols(maker.shape(shardNumber).header.symbols),
          buffer(mostRecordsPerWrite * replay::logRecordBytes),
          next(firstPosition) {
        begin(maker.first(shardNumber, firstPosition));
    }

    void LogWriter::begin(MadeLogFile&& made) {
        name = std::move(made.name);
        file = std::move(made.file);
        room = made.records;
        offset = made.headerBytes;
        checksum = made.chainSeed;  // each file's records chain from its own header
    }

    // Writes with fixed buffers only, as the matching thread's abort does.
    void LogWriter::abortOnFailure(const char* what) const {
        std::fprintf(stderr, "order book runtime: shard %u log writer: %s: %s\n", static_cast<unsigned int>(shard), name.c_str(), what);
        std::fflush(stderr);
        std::abort();
    }

    std::size_t LogWriter::pass() {
        if (finished) {
            return 0;
        }
        if (!file) {
            // The last file is full and closed, so a STOP has nothing left to finish, and a
            // record needs the next file.
            if (ring.waiting(1) == 0) {
                return 0;
            }
            if (ring.peek(0).control == ControlKind::STOP) {
                ring.drain(1, [](const LogEntry&) {});
                finished = true;
                return 0;
            }
            std::optional<MadeLogFile> taken = housekeeping.takeNext(shard);
            if (!taken) {
                waiting = true;  // the drive is slow: the ring fills, and connections park
                return 0;
            }
            waiting = false;
            begin(std::move(*taken));
        }
        std::size_t records = 0;
        bool stopping = false;
        const std::size_t limit = static_cast<std::size_t>(std::min<std::uint64_t>(mostRecordsPerWrite, room));
        ring.drain(limit, [&](const LogEntry& entry) {
            if (entry.control == ControlKind::STOP) {
                stopping = true;  // the matching thread's last
                return;
            }
            checksum = replay::encodeLogRecord(symbols[entry.slot], entry.command, checksum,
                                               std::span<std::byte, replay::logRecordBytes>(buffer.data() + records * replay::logRecordBytes,
                                                                                            replay::logRecordBytes));
            ++records;
        });
        try {
            if (records != 0) {
                const std::size_t bytes = records * replay::logRecordBytes;
                file->write(offset, std::span<const std::byte>(buffer).first(bytes));
                offset += bytes;
                room -= records;
                next += records;
                durable.publish(next);
            }
            if (room == 0) {
                // Before any write to the next, so only the last file can have an unsynced tail.
                file->flush();
                file->close();
                file.reset();
            }
            if (stopping) {
                if (file) {
                    // A clean stop leaves no zero tail, which would read as a crash.
                    file->cut(offset);
                    file->flush();
                    file->close();
                    file.reset();
                }
                finished = true;
            }
        } catch (const std::exception& failure) {
            abortOnFailure(failure.what());
        } catch (...) {
            abortOnFailure("an unknown exception");
        }
        return records;
    }

    void LogWriter::run(Doorbell& doorbell, std::chrono::nanoseconds spinWindow) {
        using Clock = std::chrono::steady_clock;
        bool idle = false;
        Clock::time_point idleSince;
        while (!finished) {
            if (pass() != 0 || finished) {
                idle = false;
                continue;
            }
            if (waiting) {
                if (!housekeeping.waitForNext(shard)) {
                    // Housekeeping stopped first, which only an engine abandoning its threads
                    // does: nothing can come, so yield rather than spin.
                    std::this_thread::yield();
                }
                idle = false;
                continue;
            }
            if (spinWindow != std::chrono::nanoseconds::zero()) {
                const Clock::time_point now = Clock::now();
                if (!idle) {
                    idle = true;
                    idleSince = now;
                }
                if (spinWindow == std::chrono::nanoseconds::max() || now - idleSince < spinWindow) {
                    std::this_thread::yield();
                    continue;
                }
            }
            doorbell.announceSleep();
            if (!ring.hasItems()) {
                doorbell.wait();
            }
            doorbell.cancelSleep();
            idle = false;
        }
    }

}  // namespace order_book::runtime
