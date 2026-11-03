#include "order_book/runtime/log_writer.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <span>
#include <thread>

namespace order_book::runtime {

    namespace {
        replay::LogHeader headerOf(const InstrumentDirectory& directory, std::uint32_t shard, std::uint64_t firstPosition) {
            replay::LogHeader header;
            header.engineMajor = ORDER_BOOK_VERSION_MAJOR;
            header.engineMinor = ORDER_BOOK_VERSION_MINOR;
            header.enginePatch = ORDER_BOOK_VERSION_PATCH;
            header.firstPosition = firstPosition;
            header.shardCount = directory.shardCount();
            header.shardIndex = shard;
            for (const Instrument& instrument : directory.onShard(shard)) {
                header.symbols.push_back(instrument.symbol);  // a shard's slots are in symbol order
            }
            return header;
        }
    }  // namespace

    LogWriter::LogWriter(LogStorage& storage, const InstrumentDirectory& directory, std::uint32_t shardNumber, Durability durability,
                         Ring<LogEntry>& logRing, DurablePosition& durablePosition, std::uint64_t firstPosition)
        : ring(logRing),
          durable(durablePosition),
          shard(shardNumber),
          name(logFileName(shardNumber, firstPosition)),
          buffer(mostRecordsPerWrite * replay::logRecordBytes),
          next(firstPosition) {
        const replay::LogHeader header = headerOf(directory, shard, firstPosition);
        symbols = header.symbols;
        const std::vector<std::byte> encoded = replay::encodeLogHeader(header);
        checksum = replay::logChainSeed(encoded);
        file = storage.create(name, durability);
        try {
            file->write(0, encoded);
        } catch (...) {
            // A writer that never started leaves no file, so the next start isn't refused on it.
            file.reset();
            try {
                storage.remove(name);
            } catch (...) {
            }
            throw;
        }
        offset = encoded.size();
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
        std::size_t records = 0;
        bool stopping = false;
        ring.drain(mostRecordsPerWrite, [&](const LogEntry& entry) {
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
                next += records;
                durable.publish(next);
            }
            if (stopping) {
                file->flush();
                file->cut(offset);
                file->close();
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
