#include "order_book/runtime/book_set.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <span>

#include "order_book/invariants.hpp"

namespace order_book::runtime {

    BookSet::BookSet(const InstrumentDirectory& directory, std::uint32_t shardNumber, const BookCapacity& defaults)
        : shardIndex(shardNumber), owningThread(std::this_thread::get_id()) {
        const std::span<const Instrument> instruments = directory.onShard(shardNumber);
        books.reserve(instruments.size());
        for (const Instrument& instrument : instruments) {
            books.emplace_back();
            books.back().reserve(capacityOf(instrument, defaults));
        }
    }

    void BookSet::checkCaller(const char* call, std::uint32_t slot) const {
        if constexpr (invariantChecksAfterCommands) {
            if (std::this_thread::get_id() != owningThread) {
                std::cerr << "order book runtime: BookSet::" << call << " called off its owning thread: shard=" << shardIndex
                          << " slot=" << slot << std::endl;
                std::abort();
            }
        }
        assert(slot < books.size());
    }

    replay::CommandOutcome BookSet::execute(std::uint32_t slot, const replay::Command& command) {
        checkCaller("execute", slot);
        return replay::executeCommand(books[slot], command);
    }

    void BookSet::execute(std::uint32_t slot, const replay::Command& command, replay::CommandResult& out) {
        checkCaller("execute", slot);
        replay::executeCommand(books[slot], command, out);
    }

    const OrderBook& BookSet::book(std::uint32_t slot) const {
        checkCaller("book", slot);
        return books[slot];
    }

}  // namespace order_book::runtime
