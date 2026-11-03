#pragma once

#include <atomic>
#include <cstdint>

namespace order_book {
    struct OrderBookTestAccess;
}

namespace order_book::runtime {

    // Wakes a sleeping thread: one per matching thread, and one per log writer. A producer rings
    // after each batch.
    //
    // Every change to the one atomic is an exchange, sequentially consistent and never a fence,
    // since GCC's -Wtsan fails a fence under warnings as errors. That is what rules out a lost
    // wake-up. Exchanges on one atomic form a single order. If a producer's comes after the
    // sleeper's it reads asleep, and wakes it. If it comes before, the sleeper's exchange reads
    // what it wrote, or a later exchange in that unbroken chain, and so synchronizes with it, and
    // the sleeper's re-check sees everything pushed before the ring. A plain store anywhere would
    // break the chain, and a producer that only read a flag could have its load pass its own
    // earlier push.
    class Doorbell {
    public:
        void ring() noexcept {
            if (state.exchange(awake, std::memory_order_seq_cst) == asleep) {
                state.notify_one();
            }
        }

        // The sleeper's half. After this it must look at its rings again: if they hold anything
        // it calls cancelSleep, and otherwise wait, then cancelSleep.
        void announceSleep() noexcept { state.exchange(asleep, std::memory_order_seq_cst); }
        void cancelSleep() noexcept { state.exchange(awake, std::memory_order_seq_cst); }
        void wait() noexcept { state.wait(asleep, std::memory_order_seq_cst); }

    private:
        friend struct order_book::OrderBookTestAccess;

        static constexpr std::uint32_t awake = 0;
        static constexpr std::uint32_t asleep = 1;

        std::atomic<std::uint32_t> state{awake};
    };

}  // namespace order_book::runtime
