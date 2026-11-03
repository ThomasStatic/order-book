#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "order_book/runtime/doorbell.hpp"
#include "order_book/runtime/log_writer.hpp"
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

    // One producer's rings: its commands in, and their responses back.
    struct RingPair {
        Ring<CommandElement>* commands;
        Ring<ResponseRecord>* responses;
    };

    // A shard's log ring, which only its matching thread writes, and its log writer's doorbell.
    struct LogLink {
        Ring<LogEntry>* ring;
        Doorbell* writerBell;
    };

    // What a matching thread does, as a plain object, so a test can step it on one thread and the
    // thread itself is run. The take order is the command sequence. It belongs to the shard's
    // thread, and the rings and doorbell must outlive it.
    class MatchingLoop {
    public:
        // waitInPlace runs each time a full response ring or log ring holds this thread up, in
        // place of a yield, so a test stepping every actor on one thread can play the others
        // there. With a log, every command taken goes to the log ring, and its response is held.
        MatchingLoop(Shard& shard, Doorbell& doorbell, std::vector<RingPair> rings,
                     std::chrono::nanoseconds spinWindow = defaultSpinWindow, std::function<void()> waitInPlace = {},
                     std::optional<LogLink> log = std::nullopt);

        // One pass: up to takeBatch commands from each ring in turn, each run on the shard, with
        // its events sent back on its own ring's responses. A full response ring makes this wait
        // in place. Returns the commands taken. An exception escaping a command aborts the
        // process, naming the shard, instrument, and command: the book may have executions
        // standing that its log can't reproduce, so carrying on would break determinism.
        //
        // With a log, a take is no larger than the log ring, and before it runs, the matching
        // thread waits for room for all of it there, copies its commands in, rings the log
        // writer, and puts a HOLD marker on the response ring naming the position they all
        // need. So the writer can make every one durable while this thread waits mid-take for
        // response room: otherwise a gateway held at the marker could never make that room.
        std::size_t pass();

        // Sleeps until a producer rings, unless a command is already waiting. run calls this once
        // its rings have stayed empty for the spin window.
        void sleepUntilRung();

        // Passes until every producer has sent a stop marker, spinning for the spin window, then
        // sleeping, on empty rings. Then tells the log writer to finish, and sends SHARD_STOPPED
        // on every response ring, behind its last marker.
        void run();

        bool stopped() const noexcept { return stopMarkers == rings.size(); }

    private:
        void take(const CommandElement& element, Ring<ResponseRecord>& responses);
        void push(Ring<ResponseRecord>& responses, const ResponseRecord& record);
        void waitAWhile();
        std::size_t takeLogged(const RingPair& pair);
        bool commandsWaiting() const noexcept;

        Shard& shard;
        Doorbell& doorbell;
        std::vector<RingPair> rings;
        std::chrono::nanoseconds spinWindow;
        std::function<void()> waitInPlace;
        std::optional<LogLink> log;
        std::size_t stopMarkers = 0;
    };

}  // namespace order_book::runtime
