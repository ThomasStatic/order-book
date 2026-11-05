#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "order_book/command.hpp"
#include "order_book/invariants.hpp"
#include "order_book/runtime/book_set.hpp"
#include "order_book/runtime/durable_position.hpp"
#include "order_book/runtime/instruments.hpp"

namespace order_book {
    struct OrderBookTestAccess;
}

namespace order_book::runtime {

    // What Shard::checkInvariants found broken.
    enum class ShardViolationKind {
        BOOK,               // a book's own check failed
        SLOT_MAPPING,       // a slot does not map back through the directory
        LOG_POSITION,       // the next log position is not the first plus the commands taken
        DURABLE_POSITION    // the durable position passed the next position, or went back
    };

    struct ShardViolation {
        ShardViolationKind kind;
        std::uint32_t slot;  // 0 when it names no slot
        std::string message;
    };

    // Where a shard stood when it took a command, for the engine's abort handler.
    struct ShardProgress {
        std::uint32_t slot = 0;
        std::uint64_t position = 0;

        bool operator==(const ShardProgress&) const = default;
    };

    // A plain single-threaded object, with the matching thread a thin loop around it. process
    // takes a command and slot with no origin, so replay and tests call exactly what the matching
    // thread calls. Like the BookSet it holds, it belongs to the thread that built it.
    class Shard {
    public:
        static constexpr std::size_t resultTrades = 512;
        static constexpr std::size_t resultEvents = 1024;

        // directory must outlive this. firstPosition is the log position of the first command.
        Shard(const InstrumentDirectory& directory, std::uint32_t shardNumber, const BookCapacity& defaults = defaultBookCapacity,
              std::uint64_t firstPosition = 0);

        Shard(const Shard&) = delete;
        Shard& operator=(const Shard&) = delete;

        // Runs one command on this slot's book, leaving its events in result(). slot must be
        // below size(). Checked builds record the slot and position first.
        void process(const replay::Command& command, std::uint32_t slot);

        // A record its book already holds, as recovery from checkpoints passes one: the position
        // moves on, the command counts as taken, and nothing runs.
        void skip() noexcept {
            ++nextLogPosition;
            ++taken;
        }

        // Where the log starts for this shard, for recovery, which knows it only once each book is
        // restored. Before any command is taken: it throws std::logic_error after one.
        void startAt(std::uint64_t firstPosition);

        const replay::CommandResult& result() const noexcept { return lastResult; }
        // How many commands made the reused result reach the heap, since it was built: zero for a
        // shard whose results stayed inside what it reserved.
        std::uint64_t resultGrowths() const noexcept { return growths; }
        const BookSet& books() const noexcept { return bookSet; }
        // Warms a slot's book further, which recovery does once a book holds what it replayed.
        void reserve(std::uint32_t slot, const BookCapacity& capacity) { bookSet.reserve(slot, capacity); }
        // Puts a book OrderBook::restore rebuilt on this thread in a slot, as BookSet::replace does.
        void replaceBook(std::uint32_t slot, OrderBook book) noexcept { bookSet.replace(slot, std::move(book)); }
        std::size_t size() const noexcept { return bookSet.size(); }

        std::uint64_t nextPosition() const noexcept { return nextLogPosition; }
        std::uint64_t commandsTaken() const noexcept { return taken; }

        // In checked builds, the command process took last or is running. Zeroes otherwise.
        ShardProgress lastProgress() const noexcept { return progress; }

        // The durable position this shard's log writer publishes, which must outlive it, for
        // checkInvariants to hold to what this shard has taken.
        void watchDurablePosition(const DurablePosition& position) noexcept {
            durable = &position;
            durableSeen = logStart;
        }

        // Every book's own check, that each slot maps back through the directory to this shard
        // and slot, that the next log position is the first plus the commands taken, and, once
        // a durable position is watched, that it never passes the next position or goes back,
        // from the first position or from what an earlier check saw. That last holds wherever
        // no take is half run, since a whole take reaches the log ring before any of it runs,
        // so mid-take the log writer may already have made the rest durable.
        std::optional<ShardViolation> checkInvariants() const;

    private:
        friend struct order_book::OrderBookTestAccess;

        const InstrumentDirectory* instruments;
        BookSet bookSet;
        replay::CommandResult lastResult;
        std::uint64_t growths = 0;
        std::uint64_t logStart;
        std::uint64_t nextLogPosition;
        std::uint64_t taken = 0;
        ShardProgress progress;
        const DurablePosition* durable = nullptr;
        mutable std::uint64_t durableSeen = 0;  // the most an earlier check saw
    };

}  // namespace order_book::runtime
