#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

#include "benchmark_log.hpp"
#include "clock.hpp"
#include "order_book/command_log.hpp"
#include "order_book/runtime/durability.hpp"
#include "order_book/runtime/log_storage.hpp"
#include "timetable.hpp"
#include "workload.hpp"

// The raw write loop: each command one record's bytes written at the next offset of one file,
// made as a run's log files are and opened as the log writer opens them, at process or machine
// durability, with no log writer, ring, doorbell, or chain. A second file is made beside it and
// never written, as a run's log makes its next file ahead. Beside the durable wait at the same
// rate it tells a stall of the drive or the OS, which both show, from one of the engine's, which
// only the durable wait does.
namespace order_book::latency {

class RawWriteTarget {
public:
    // Makes the file in a fresh directory inside logParent, untimed: extended, or zero-filled and
    // flushed, to fileBytes, a log file's size, or more if every command needs it. Throws
    // std::invalid_argument for NONE.
    RawWriteTarget(const benchmark::Workload& workload, benchmark::RunDurability durability, const std::filesystem::path& logParent,
                   benchmark::LogFileFill fill, std::uint64_t fileBytes = runtime::defaultLogFileBytes);
    ~RawWriteTarget();

    RawWriteTarget(const RawWriteTarget&) = delete;
    RawWriteTarget& operator=(const RawWriteTarget&) = delete;

    template <typename DriverClock>
    Stamps operator()(std::size_t index, DriverClock& clock) {
        fillRecord(record_, index);
        const Ticks started = clock.now();
        file_->write(static_cast<std::uint64_t>(index) * record_.size(), record_);
        const Ticks done = clock.now();
        ++written_;
        return {started, done};
    }

    // Closes the file and reads it back. Throws std::runtime_error unless it holds every record
    // written, in order.
    void checkOutcome();

    std::uint64_t capacityGrowths() const { return 0; }  // no book runs

    // Record index's bytes: the index's eight bytes, repeated.
    static void fillRecord(std::array<std::byte, replay::logRecordBytes>& record, std::size_t index);

private:
    std::filesystem::path directory_;
    std::unique_ptr<runtime::LogStorage> storage_;
    std::unique_ptr<runtime::LogFile> file_;
    std::array<std::byte, replay::logRecordBytes> record_{};
    std::size_t written_ = 0;
};

}  // namespace order_book::latency
