#pragma once

#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "order_book/command.hpp"
#include "order_book/order_book.hpp"
#include "order_book/runtime/instruments.hpp"

namespace order_book::runtime {

    // One shard's books, by slot, for one thread only: the thread that builds it, which stays
    // the only thread allowed to touch them until they are destroyed. Nothing locks
    // a book, so any other reader would be a data race. Checked builds abort on a call from a
    // different thread, naming the shard and slot.
    class BookSet {
    public:
        // A book for each instrument on this shard, warmed to its capacity. An allocation
        // failure propagates, with nothing built.
        BookSet(const InstrumentDirectory& directory, std::uint32_t shardNumber,
                const BookCapacity& defaults = defaultBookCapacity);

        // Its thread is the one that built it, so a set never moves or copies.
        BookSet(const BookSet&) = delete;
        BookSet& operator=(const BookSet&) = delete;

        std::uint32_t shard() const noexcept { return shardIndex; }
        std::size_t size() const noexcept { return books.size(); }

        // The instrument this slot's book was built for, which outlives any directory.
        const Symbol& symbol(std::uint32_t slot) const noexcept { return symbols[slot]; }

        // slot must be below size().
        replay::CommandOutcome execute(std::uint32_t slot, const replay::Command& command);
        // Overwrites out, as executeCommand's out overload does.
        void execute(std::uint32_t slot, const replay::Command& command, replay::CommandResult& out);

        const OrderBook& book(std::uint32_t slot) const;

    private:
        void checkCaller(const char* call, std::uint32_t slot) const;

        std::uint32_t shardIndex;
        std::vector<OrderBook> books;
        std::vector<Symbol> symbols;  // by slot
        std::thread::id owningThread;
    };

}  // namespace order_book::runtime
