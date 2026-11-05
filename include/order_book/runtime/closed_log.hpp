#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

// What each shard's log writer has closed, for the roll-forward workers (ADR 0014): a file is
// closed, flushed and never written again, once the next one starts at or before its shard's end
// here. One writer per shard sets it; any thread reads it. Not public API. See
// docs/internals/runtime.md.
namespace order_book::runtime {

    class ClosedLog {
    public:
        explicit ClosedLog(std::size_t shards) : ends(std::make_unique<std::atomic<std::uint64_t>[]>(shards)), count(shards) {}

        ClosedLog(const ClosedLog&) = delete;
        ClosedLog& operator=(const ClosedLog&) = delete;

        std::size_t shards() const noexcept { return count; }

        // Where the shard's writer starts, every file before it closed. Never allocates.
        void started(std::uint32_t shard, std::uint64_t position) noexcept { closed(shard, position); }

        // The writer flushed and closed a full file ending here, then wakes any waiting worker.
        // Never allocates or throws.
        void closed(std::uint32_t shard, std::uint64_t end) noexcept {
            ends[shard].store(end, std::memory_order_release);
            {
                const std::lock_guard<std::mutex> held(lock);
                ++changes;
            }
            changed.notify_all();
        }

        std::uint64_t end(std::uint32_t shard) const noexcept { return ends[shard].load(std::memory_order_acquire); }

        // How many times any shard has closed a file: a worker that saw this many waits for more.
        std::uint64_t generation() const {
            const std::lock_guard<std::mutex> held(lock);
            return changes;
        }

        // Returns once a file closes after `seen` changes, or once stopping.
        void waitPast(std::uint64_t seen) const {
            std::unique_lock<std::mutex> held(lock);
            changed.wait(held, [&] { return changes != seen || stopFlag.load(std::memory_order_relaxed); });
        }

        void stop() noexcept {
            {
                const std::lock_guard<std::mutex> held(lock);
                stopFlag.store(true, std::memory_order_relaxed);
            }
            changed.notify_all();
        }

        const std::atomic<bool>& stopping() const noexcept { return stopFlag; }

    private:
        std::unique_ptr<std::atomic<std::uint64_t>[]> ends;
        std::size_t count;
        mutable std::mutex lock;
        mutable std::condition_variable changed;
        std::uint64_t changes = 0;
        std::atomic<bool> stopFlag{false};
    };

}  // namespace order_book::runtime
