#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <thread>

#include "order_book/runtime/doorbell.hpp"
#include "order_book/runtime/durability.hpp"
#include "order_book/runtime/durable_position.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/log_files.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "order_book/runtime/log_writer.hpp"
#include "order_book/runtime/matching_loop.hpp"
#include "order_book/runtime/ring.hpp"

// A shard's log for the queue and gateway kinds: the engine's own log writer and housekeeping
// thread on real files, each run in a fresh directory, so a durable run pays what the engine pays.
namespace order_book::benchmark {

// NONE is a benchmark-only shard with no log attached. PROCESS and MACHINE are the engine's
// --durability levels.
enum class RunDurability { NONE, PROCESS, MACHINE };

const char* durabilityName(RunDurability durability);
// Throws std::invalid_argument on a name other than none, process, or machine.
RunDurability parseDurability(std::string_view name);

// How a run's log files are made: EXTENDED, as the engine makes them, past their written part
// without writing it, or ZERO_FILLED, every byte written once as zeros and flushed before any
// record, so no record write lands past the file's valid data. A measurement compares the two.
enum class LogFileFill { EXTENDED, ZERO_FILLED };

const char* logFileFillName(LogFileFill fill);
// Throws std::invalid_argument on a name other than extended or zero-filled.
LogFileFill parseLogFileFill(std::string_view name);

// Storage over another whose files, when extended, are zero-filled instead: the bytes from the
// file's size to the new one written as zeros, then flushed. Everything else passes through.
class ZeroFillingLogStorage final : public runtime::LogStorage {
public:
    explicit ZeroFillingLogStorage(std::unique_ptr<runtime::LogStorage> files) : inner(std::move(files)) {}

    std::unique_ptr<runtime::StorageLock> lock() override { return inner->lock(); }
    std::vector<std::string> names() const override { return inner->names(); }
    std::unique_ptr<runtime::LogFile> open(const std::string& name) override;
    std::unique_ptr<runtime::LogFile> create(const std::string& name, runtime::Durability durability) override;
    void remove(const std::string& name) override { inner->remove(name); }
    std::string describe(const std::string& name) const override { return inner->describe(name); }

private:
    std::unique_ptr<runtime::LogStorage> inner;
};

// Real files in directory, which must exist, made as fill says.
std::unique_ptr<runtime::LogStorage> runLogStorage(const std::filesystem::path& directory, LogFileFill fill);

// The half of a shard's log a matching loop takes when it is built: the log ring, the writer's
// doorbell, and the durable position, which a producer's hold reads. The writer comes later, once
// the shard's first position is known.
struct LogRing {
    explicit LogRing(std::size_t records = runtime::defaultLogRingRecords) : ring(records) {}

    runtime::LogLink link() noexcept { return runtime::LogLink{&ring, &writerBell}; }
    // Tells the writer to finish, from the ring's one producer, waiting for room if it must.
    void sendStop() {
        while (!ring.tryPush(runtime::LogEntry::stop())) {
            std::this_thread::yield();
        }
        writerBell.ring();
    }

    runtime::Ring<runtime::LogEntry> ring;
    runtime::Doorbell writerBell;
    runtime::DurablePosition durable;
};

// Shard 0's log writer on its own thread and the housekeeping thread, over real files at one
// durability in a fresh directory made inside parent. The first file and the next are both made
// before the constructor returns, so a run shorter than two files makes none while it is timed.
// The matching thread must stop, which sends the writer its STOP, before finish or the destructor,
// and the directory is deleted with everything in it once the files are closed.
class RunLog {
public:
    // directory and ring must outlive this. setUpWriter, if given, runs first on the writer's
    // thread, for pinning it. Throws what stops the writer's first file, and std::invalid_argument
    // for NONE.
    RunLog(const std::filesystem::path& parent, RunDurability durability, const runtime::InstrumentDirectory& directory,
           LogRing& ring, std::uint64_t firstPosition, std::function<void()> setUpWriter = {},
           LogFileFill fill = LogFileFill::EXTENDED);
    ~RunLog();

    RunLog(const RunLog&) = delete;
    RunLog& operator=(const RunLog&) = delete;

    // Once the matching thread has stopped: waits for the writer to finish its file, stops
    // housekeeping, and deletes the unused next file. Returns the position the log ended at.
    std::uint64_t finish();
    const std::filesystem::path& directory() const noexcept { return where; }

private:
    std::filesystem::path where;
    std::unique_ptr<runtime::LogStorage> storage;
    std::unique_ptr<runtime::Housekeeping> housekeeping;
    LogRing& log;
    // A member, since the writer may still be inside set_value when the constructor's get returns.
    std::promise<void> ready;
    std::thread writer;
    std::thread housekeeper;
    std::uint64_t logEnd = 0;
    bool finished = false;
};

// Where a run's log directory goes unless --log-dir says: the system's temporary directory.
std::filesystem::path defaultLogParent();

// Makes a new, empty directory inside parent for one run's log, named for this process's clock and
// a count, and returns it. Throws std::runtime_error if it can't.
std::filesystem::path freshLogDirectory(const std::filesystem::path& parent);

}  // namespace order_book::benchmark
