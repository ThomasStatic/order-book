#pragma once

#include <atomic>
#include <cstdint>

namespace order_book::runtime {

    // How far a shard's log is safe at its durability level: every position below it is (ADR
    // 0013). One writer, the shard's log writer, and any number of readers: the gateway threads,
    // and the shard's own check.
    class DurablePosition {
    public:
        explicit DurablePosition(std::uint64_t start = 0) noexcept : position(start) {}

        DurablePosition(const DurablePosition&) = delete;
        DurablePosition& operator=(const DurablePosition&) = delete;

        std::uint64_t load() const noexcept { return position.load(std::memory_order_acquire); }
        void publish(std::uint64_t next) noexcept { position.store(next, std::memory_order_release); }

    private:
        std::atomic<std::uint64_t> position;
    };

}  // namespace order_book::runtime
