#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "benchmark_log.hpp"
#include "benchmark_runner.hpp"
#include "clock.hpp"
#include "order_book/command_log.hpp"
#include "order_book/runtime/instruments.hpp"
#include "order_book/runtime/log_writer.hpp"
#include "timetable.hpp"
#include "workload.hpp"

// The durable-wait interval: from a record entering a shard's log ring until the durable position
// covers it, on the one thread that puts it there, through the engine's own log writer on its own
// thread, writing real files at process or machine durability. No book runs: the records are the
// workload's commands, as a matching thread would put them on the ring, so what is timed is the
// log alone. A target waits for each record before the next, as a hand-off waits for each answer.
namespace order_book::latency {

class DurableWaitTarget {
public:
    // Starts the log writer, which runs setUpWriter first, for pinning it, on files in a fresh
    // directory inside logParent, made as fill says, untimed: the engine's 256 MiB each, or
    // fileRecords records past the header. With rollWorker, a roll-forward worker rolls each file
    // the writer closes, from an empty book's checkpoint at position 0, while the run goes on: it
    // and the housekeeping thread then run setUpWriter too. The workload is read here only.
    // Throws std::invalid_argument for NONE, and what stops the first file being made.
    DurableWaitTarget(const benchmark::Workload& workload, benchmark::RunDurability durability, const std::filesystem::path& logParent,
                      std::function<void()> setUpWriter = {}, benchmark::LogFileFill fill = benchmark::LogFileFill::EXTENDED,
                      bool rollWorker = false, std::optional<std::uint64_t> fileRecords = std::nullopt);
    ~DurableWaitTarget();

    DurableWaitTarget(const DurableWaitTarget&) = delete;
    DurableWaitTarget& operator=(const DurableWaitTarget&) = delete;

    template <typename DriverClock>
    Stamps operator()(std::size_t index, DriverClock& clock) {
        const runtime::LogEntry entry{records_[index].command, 0, runtime::ControlKind::COMMAND};
        const std::uint64_t covered = index + 1;  // the position past this record
        const Ticks started = clock.now();
        // The ring has room, since every earlier record was waited for, so this never spins.
        while (!ring_.ring.tryPush(entry)) {
        }
        ring_.writerBell.ring();
        while (ring_.durable.load() < covered) {
        }
        const Ticks done = clock.now();
        ++sent_;
        return {started, done};
    }

    // Stops the log writer and reads its files back through CommandLogReader. Throws
    // std::runtime_error unless they hold exactly the records sent, in order, or with a worker,
    // whose rolls let retention delete what no checkpoint needs, the last of them.
    void checkOutcome();

    std::uint64_t capacityGrowths() const { return 0; }  // no book runs
    // The worker's passes while the run went on, after checkOutcome; none without one.
    std::uint64_t rollPasses() const noexcept { return passes_; }
    // Every log position below this is durable.
    std::uint64_t durablePosition() const noexcept { return ring_.durable.load(); }

private:
    std::vector<replay::ReplayRecord> records_;
    runtime::InstrumentDirectory directory_;
    benchmark::LogRing ring_;
    std::unique_ptr<benchmark::RunLog> log_;
    std::uint64_t passes_ = 0;
    bool rollWorker_ = false;
    std::size_t sent_ = 0;
};

}  // namespace order_book::latency
