#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "order_book/runtime/ring.hpp"
#include "order_book/runtime/shard.hpp"

namespace order_book {
    struct OrderBookTestAccess;
}

namespace order_book::runtime {

    // How long a matching thread spins on empty rings before it sleeps. 0 sleeps at once, and
    // unlimitedSpin never sleeps, at one core per shard while idle.
    inline constexpr std::chrono::nanoseconds defaultSpinWindow = std::chrono::microseconds(100);
    inline constexpr std::chrono::nanoseconds unlimitedSpin = std::chrono::nanoseconds::max();

    // How many commands a matching thread takes from one ring before it moves to the next.
    inline constexpr std::size_t takeBatch = 64;

    // Wakes a sleeping matching thread: one per shard. A producer rings after each batch.
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

    // One producer's rings: its commands in, and their responses back.
    struct RingPair {
        Ring<CommandElement>* commands;
        Ring<ResponseRecord>* responses;
    };

    // What a matching thread does, as a plain object, so a test can step it on one thread and the
    // thread itself is run. The take order is the command sequence. It belongs to the shard's
    // thread, and the rings and doorbell must outlive it.
    class MatchingLoop {
    public:
        // waitInPlace runs each time a full response ring holds this thread up, in place of a
        // yield, so a test stepping every actor on one thread can play the others there.
        MatchingLoop(Shard& shard, Doorbell& doorbell, std::vector<RingPair> rings,
                     std::chrono::nanoseconds spinWindow = defaultSpinWindow, std::function<void()> waitInPlace = {});

        // One pass: up to takeBatch commands from each ring in turn, each run on the shard, with
        // its events sent back on its own ring's responses. A full response ring makes this wait
        // in place. Returns the commands taken. An exception escaping a command aborts the
        // process, naming the shard, instrument, and command: the book may have executions
        // standing that its log can't reproduce, so carrying on would break determinism.
        std::size_t pass();

        // Sleeps until a producer rings, unless a command is already waiting. run calls this once
        // its rings have stayed empty for the spin window.
        void sleepUntilRung();

        // Passes until every producer has sent a stop marker, spinning for the spin window, then
        // sleeping, on empty rings. Then sends SHARD_STOPPED on every response ring.
        void run();

        bool stopped() const noexcept { return stopMarkers == rings.size(); }

    private:
        void take(const CommandElement& element, Ring<ResponseRecord>& responses);
        void push(Ring<ResponseRecord>& responses, const ResponseRecord& record);
        bool commandsWaiting() const noexcept;

        Shard& shard;
        Doorbell& doorbell;
        std::vector<RingPair> rings;
        std::chrono::nanoseconds spinWindow;
        std::function<void()> waitInPlace;
        std::size_t stopMarkers = 0;
    };

}  // namespace order_book::runtime
