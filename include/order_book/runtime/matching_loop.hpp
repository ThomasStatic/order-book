#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "order_book/runtime/ring.hpp"
#include "order_book/runtime/shard.hpp"

namespace order_book::runtime {

    // How long a matching thread spins on empty rings before it sleeps. 0 sleeps at once, and
    // unlimitedSpin never sleeps, at one core per shard while idle.
    inline constexpr std::chrono::nanoseconds defaultSpinWindow = std::chrono::microseconds(100);
    inline constexpr std::chrono::nanoseconds unlimitedSpin = std::chrono::nanoseconds::max();

    // How many commands a matching thread takes from one ring before it moves to the next.
    inline constexpr std::size_t takeBatch = 64;

    // Wakes a sleeping matching thread: one per shard. A producer rings after each batch.
    //
    // Both sides exchange the one atomic, sequentially consistent and never a fence, since GCC's
    // -Wtsan fails a fence under warnings as errors. Sharing the atomic is what rules out a lost
    // wake-up. If a producer's exchange comes second it sees the sleeper and wakes it. If it comes
    // first, the sleeper's exchange reads what it wrote, and so sees the commands pushed before
    // it, on its re-check. A producer that only read a flag would not be enough, since its load
    // could pass its own earlier push to the ring.
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
        void cancelSleep() noexcept { state.store(awake, std::memory_order_seq_cst); }
        void wait() noexcept { state.wait(asleep, std::memory_order_seq_cst); }

        // Whether the sleeper has announced itself, so a test can catch it there.
        bool sleeperAnnounced() const noexcept { return state.load(std::memory_order_seq_cst) == asleep; }

    private:
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
        MatchingLoop(Shard& shard, Doorbell& doorbell, std::vector<RingPair> rings,
                     std::chrono::nanoseconds spinWindow = defaultSpinWindow);

        // One pass: up to takeBatch commands from each ring in turn, each run on the shard, with
        // its events sent back on its own ring's responses. A full response ring makes this wait
        // in place. Returns the commands taken. An exception escaping a command aborts the
        // process, naming the shard, instrument, and command: the book may have executions
        // standing that its log can't reproduce, so carrying on would break determinism.
        std::size_t pass();

        // Passes until every producer has sent a stop marker, spinning for the spin window, then
        // sleeping, on empty rings. Then sends SHARD_STOPPED on every response ring.
        void run();

        bool stopped() const noexcept { return stopMarkers == rings.size(); }

    private:
        void take(const CommandElement& element, Ring<ResponseRecord>& responses);
        bool commandsWaiting() const noexcept;

        Shard& shard;
        Doorbell& doorbell;
        std::vector<RingPair> rings;
        std::chrono::nanoseconds spinWindow;
        std::size_t stopMarkers = 0;
    };

}  // namespace order_book::runtime
