#include "order_book/runtime/log_files.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <stdexcept>
#include <utility>

namespace order_book::runtime {

    namespace {
        replay::LogHeader headerOf(const InstrumentDirectory& directory, std::uint32_t shard) {
            replay::LogHeader header;
            header.engineMajor = ORDER_BOOK_VERSION_MAJOR;
            header.engineMinor = ORDER_BOOK_VERSION_MINOR;
            header.enginePatch = ORDER_BOOK_VERSION_PATCH;
            header.shardCount = directory.shardCount();
            header.shardIndex = shard;
            for (const Instrument& instrument : directory.onShard(shard)) {
                header.symbols.push_back(instrument.symbol);  // a shard's slots are in symbol order
            }
            return header;
        }

        void tryRemove(LogStorage& storage, const std::string& name) noexcept {
            try {
                storage.remove(name);
            } catch (...) {
            }
        }

        // With fixed buffers and no allocation, since the failure may be that nothing can be.
        [[noreturn]] void abortMakingNext(std::uint32_t shard, const char* reason) {
            std::fprintf(stderr, "order book runtime: housekeeping: cannot make shard %u's next log file: %s\n",
                         static_cast<unsigned int>(shard), reason);
            std::fflush(stderr);
            std::abort();
        }
    }  // namespace

    LogFileShape logFileShape(const InstrumentDirectory& directory, std::uint32_t shard, Durability durability, std::uint64_t fileBytes) {
        LogFileShape shape;
        shape.shard = shard;
        shape.header = headerOf(directory, shard);
        shape.durability = durability;
        shape.headerBytes = replay::encodeLogHeader(shape.header).size();
        if (fileBytes < shape.headerBytes + replay::logRecordBytes) {
            throw std::invalid_argument("a log file of " + std::to_string(fileBytes) + " bytes can't hold shard " + std::to_string(shard) +
                                        "'s header and one record, " + std::to_string(shape.headerBytes + replay::logRecordBytes) +
                                        " bytes");
        }
        shape.records = (fileBytes - shape.headerBytes) / replay::logRecordBytes;
        return shape;
    }

    std::uint64_t leastLogFileBytes(const InstrumentDirectory& directory) {
        std::uint64_t least = 0;
        for (std::uint32_t shard = 0; shard < directory.shardCount(); ++shard) {
            const std::uint64_t bytes = replay::encodeLogHeader(headerOf(directory, shard)).size() + replay::logRecordBytes;
            least = bytes > least ? bytes : least;
        }
        return least;
    }

    MadeLogFile makeLogFile(LogStorage& storage, const LogFileShape& shape, std::uint64_t firstPosition) {
        replay::LogHeader header = shape.header;
        header.firstPosition = firstPosition;
        const std::vector<std::byte> encoded = replay::encodeLogHeader(header);
        MadeLogFile made;
        made.name = logFileName(shape.shard, firstPosition);
        made.firstPosition = firstPosition;
        made.headerBytes = encoded.size();
        made.records = shape.records;
        made.chainSeed = replay::logChainSeed(encoded);
        const std::string making = made.name + ".tmp";
        made.file = storage.create(making, shape.durability);
        try {
            made.file->write(0, encoded);
            // Only extended, never written, so making a file doesn't compete with the log for
            // the drive, and nothing in it is written twice.
            made.file->extend(made.headerBytes + made.records * replay::logRecordBytes);
            made.file->flush();
        } catch (...) {
            made.file.reset();
            tryRemove(storage, making);
            throw;
        }
        try {
            made.file->rename(made.name);
        } catch (const LogStorageError& error) {
            made.file.reset();
            tryRemove(storage, making);
            if (error.kind() != LogStorageError::Kind::EXISTS) {
                tryRemove(storage, made.name);  // renamed, but its name may not last
            }
            throw;
        } catch (...) {
            made.file.reset();
            tryRemove(storage, making);
            throw;
        }
        return made;
    }

    Housekeeping::Housekeeping(LogStorage& storage, const InstrumentDirectory& directory, Durability durability, std::uint64_t fileBytes)
        : logStorage(storage), shards(directory.shardCount()) {
        shapes.reserve(directory.shardCount());
        for (std::uint32_t shard = 0; shard < directory.shardCount(); ++shard) {
            shapes.push_back(logFileShape(directory, shard, durability, fileBytes));
        }
    }

    MadeLogFile Housekeeping::first(std::uint32_t shard, std::uint64_t firstPosition) {
        MadeLogFile made = makeLogFile(logStorage, shapes.at(shard), firstPosition);
        {
            const std::lock_guard<std::mutex> held(lock);
            shards[shard].started = true;
            shards[shard].nextPosition = firstPosition + made.records;
        }
        changed.notify_all();
        return made;
    }

    std::optional<MadeLogFile> Housekeeping::takeNext(std::uint32_t shard) {
        std::optional<MadeLogFile> taken;
        {
            const std::lock_guard<std::mutex> held(lock);
            taken = std::exchange(shards.at(shard).next, std::nullopt);
        }
        if (taken) {
            changed.notify_all();  // the one after it is wanted
        }
        return taken;
    }

    bool Housekeeping::waitForNext(std::uint32_t shard) {
        std::unique_lock<std::mutex> held(lock);
        changed.wait(held, [&] { return stopping || shards.at(shard).next.has_value(); });
        return shards.at(shard).next.has_value();
    }

    std::size_t Housekeeping::pass() {
        std::size_t made = 0;
        for (std::uint32_t shard = 0; shard < shards.size(); ++shard) {
            std::uint64_t position = 0;
            {
                const std::lock_guard<std::mutex> held(lock);
                if (!wanted(shards[shard])) {
                    continue;
                }
                position = shards[shard].nextPosition;
            }
            // Outside the lock, since a log writer may take or wait meanwhile, and this is the
            // only pass, so nothing else makes this file.
            std::optional<MadeLogFile> next;
            try {
                next = makeLogFile(logStorage, shapes[shard], position);
            } catch (const std::bad_alloc&) {
                abortMakingNext(shard, "not enough memory");
            } catch (const std::exception& failure) {
                abortMakingNext(shard, failure.what());
            } catch (...) {
                abortMakingNext(shard, "an unknown exception");
            }
            {
                const std::lock_guard<std::mutex> held(lock);
                shards[shard].nextPosition = position + next->records;
                shards[shard].next = std::move(next);
            }
            changed.notify_all();
            ++made;
        }
        for (std::uint32_t shard = 0; shard < shards.size(); ++shard) {
            std::uint64_t floor = 0;
            {
                const std::lock_guard<std::mutex> held(lock);
                if (!pruneWanted(shards[shard])) {
                    continue;
                }
                floor = shards[shard].floor;
            }
            const std::uint64_t before = deleted.load(std::memory_order_relaxed);
            prune(shard, floor);
            made += static_cast<std::size_t>(deleted.load(std::memory_order_relaxed) - before);
            {
                // Even if a deletion failed: it is tried again once the floor next rises, not in a
                // loop, which would only print the same failure.
                const std::lock_guard<std::mutex> held(lock);
                shards[shard].pruned = std::max(shards[shard].pruned, floor);
            }
        }
        return made;
    }

    bool Housekeeping::prune(std::uint32_t shard, std::uint64_t floor) {
        std::vector<std::pair<std::uint64_t, std::string>> files;
        try {
            for (const std::string& name : logStorage.names()) {
                const std::optional<LogFileNameParts> parts = parseLogFileName(name);
                if (parts && !parts->temporary && parts->shard == shard) {
                    files.emplace_back(parts->firstPosition, name);
                }
            }
        } catch (const std::exception& failure) {
            std::fprintf(stderr, "order book runtime: housekeeping: cannot list shard %u's log files: %s\n", static_cast<unsigned int>(shard),
                         failure.what());
            return false;
        }
        std::sort(files.begin(), files.end());
        bool whole = true;
        for (std::size_t i = 0; i + 1 < files.size() && files[i + 1].first <= floor; ++i) {
            try {
                logStorage.remove(files[i].second);
                deleted.fetch_add(1, std::memory_order_relaxed);
            } catch (const std::exception& failure) {
                // Not fatal: the file only takes room, and the next pass tries again.
                std::fprintf(stderr, "order book runtime: housekeeping: cannot delete %s: %s\n", files[i].second.c_str(), failure.what());
                whole = false;
            }
        }
        return whole;
    }

    void Housekeeping::retain(std::uint32_t shard, std::uint64_t floor) {
        {
            const std::lock_guard<std::mutex> held(lock);
            ShardFiles& files = shards.at(shard);
            if (floor <= files.floor) {
                return;
            }
            files.floor = floor;
        }
        changed.notify_all();
    }

    void Housekeeping::keepEveryFile() noexcept {
        const std::lock_guard<std::mutex> held(lock);
        keepLog = true;
    }

    // Looks for a stop before every pass, so a stop that comes first makes nothing.
    void Housekeeping::run() {
        for (;;) {
            {
                std::unique_lock<std::mutex> held(lock);
                changed.wait(held, [&] {
                    if (stopping) {
                        return true;
                    }
                    for (const ShardFiles& shard : shards) {
                        if (wanted(shard) || pruneWanted(shard)) {
                            return true;
                        }
                    }
                    return false;
                });
                if (stopping) {
                    return;
                }
            }
            pass();
        }
    }

    void Housekeeping::stop() {
        {
            const std::lock_guard<std::mutex> held(lock);
            stopping = true;
        }
        changed.notify_all();
    }

    void Housekeeping::removeUnused() {
        std::optional<LogStorageError> failed;
        for (ShardFiles& shard : shards) {
            std::optional<MadeLogFile> unused;
            {
                const std::lock_guard<std::mutex> held(lock);
                unused = std::exchange(shard.next, std::nullopt);
            }
            if (!unused) {
                continue;
            }
            try {
                unused->file->close();
            } catch (const LogStorageError&) {
                // Removing it is what matters.
            }
            unused->file.reset();
            try {
                logStorage.remove(unused->name);
            } catch (const LogStorageError& error) {
                if (!failed) {
                    failed = error;
                }
            }
        }
        if (failed) {
            throw *failed;
        }
    }

}  // namespace order_book::runtime
