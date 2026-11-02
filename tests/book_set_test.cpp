#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "order_book/runtime/book_set.hpp"

namespace {

using namespace order_book;
using namespace order_book::runtime;
using replay::AddCommand;
using replay::CancelCommand;
using replay::Command;
using replay::CommandOutcome;
using replay::CommandResult;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Symbol symbol(std::string_view text) {
    return *Symbol::parse(text);
}

Instrument on(std::string_view text, std::uint32_t shard) {
    return Instrument{symbol(text), shard, std::nullopt, std::nullopt, std::nullopt};
}

Command add(OrderId id, Side side, Price price, Quantity quantity) {
    AddCommand command{};
    command.orderId = id;
    command.side = side;
    command.price = price;
    command.quantity = quantity;
    return command;
}

void testHoldsItsShardsBooks() {
    const InstrumentDirectory directory({on("AAPL", 1), on("MSFT", 1), on("GOOG", 0), on("AMD", 1)}, 3);
    const BookSet first(directory, 0);
    const BookSet second(directory, 1);
    const BookSet third(directory, 2);
    expect(first.size() == 1 && first.shard() == 0, "shard 0 holds one book");
    expect(second.size() == 3 && second.shard() == 1, "shard 1 holds three books");
    expect(third.size() == 0, "a shard with no instruments holds no books");
}

void testExecuteRunsTheSlotsBook() {
    const InstrumentDirectory directory({on("AAPL", 0), on("MSFT", 0)}, 1);
    BookSet set(directory, 0);
    const std::uint32_t aapl = directory.find(symbol("AAPL"))->slot;
    const std::uint32_t msft = directory.find(symbol("MSFT"))->slot;

    // The same IDs are live in both books, so a leak between books would show.
    const CommandOutcome placed = set.execute(aapl, add(1, Side::BUY, 100, 5));
    expect(std::get<SubmissionResult>(placed).getRemainingQuantity() == 5, "an ADD reaches the book and rests");
    set.execute(msft, add(1, Side::SELL, 200, 7));
    set.execute(msft, add(2, Side::SELL, 201, 1));

    expect(set.book(aapl).activeOrderCount() == 1, "AAPL's book holds what AAPL's slot was sent");
    expect(set.book(msft).activeOrderCount() == 2, "MSFT's book holds what MSFT's slot was sent");
    expect(set.book(aapl).getBestBid().getRemainingQuantity() == 5, "order 1 is AAPL's own order 1");
    expect(set.book(msft).getBestAsk().getRemainingQuantity() == 7, "order 1 is MSFT's own order 1");

    const CommandOutcome cancelled = set.execute(aapl, CancelCommand{1});
    expect(std::get<CancellationResult>(cancelled).isSuccess(), "a CANCEL reaches the slot's book");
    expect(set.book(aapl).activeOrderCount() == 0, "the cancel took AAPL's order 1");
    expect(set.book(msft).activeOrderCount() == 2, "the cancel left MSFT's order 1 alone");
}

void testTheOutOverloadMatchesExecute() {
    const InstrumentDirectory directory({on("AAPL", 0)}, 1);
    BookSet byValue(directory, 0);
    BookSet reusing(directory, 0);
    CommandResult out;

    const std::vector<Command> commands = {
        add(1, Side::SELL, 100, 5), add(2, Side::SELL, 101, 5), add(3, Side::BUY, 101, 7), CancelCommand{2}, CancelCommand{2},
        add(4, Side::SELL, 105, 9)};
    for (const Command& command : commands) {
        const CommandOutcome outcome = byValue.execute(0, command);
        reusing.execute(0, command, out);
        if (const auto* submission = std::get_if<SubmissionResult>(&outcome)) {
            expect(out.submission().getRemainingQuantity() == submission->getRemainingQuantity(), "the same remainder");
            expect(out.events().size() == submission->getEvents().size(), "the same events");
        } else {
            const auto& cancellation = std::get<CancellationResult>(outcome);
            expect(out.cancellation().isSuccess() == cancellation.isSuccess(), "the same cancellation");
        }
    }
    expect(byValue.book(0).activeOrderCount() == reusing.book(0).activeOrderCount(), "the same book");
    expect(byValue.book(0).getBestAsk().getRemainingQuantity() == reusing.book(0).getBestAsk().getRemainingQuantity(),
           "the same book");
}

void testBooksAreWarmedToCapacity() {
    Instrument listed = on("AAPL", 0);
    listed.orders = 512;
    listed.levels = 16;
    const InstrumentDirectory directory({listed, on("MSFT", 0)}, 1);
    BookSet set(directory, 0, BookCapacity{32, 8, 8});
    const std::uint32_t aapl = directory.find(symbol("AAPL"))->slot;
    const std::uint32_t msft = directory.find(symbol("MSFT"))->slot;

    for (OrderId id = 1; id <= 512; ++id) {
        set.execute(aapl, add(id, Side::BUY, 100 + id % 16, 1));
    }
    for (OrderId id = 1; id <= 32; ++id) {
        set.execute(msft, add(id, Side::BUY, 100 + id % 8, 1));
    }
    expect(set.book(aapl).capacityGrowths() == 0, "a list row's capacity is what its book was warmed to");
    expect(set.book(msft).capacityGrowths() == 0, "an empty column takes the set's default");

    set.execute(msft, add(33, Side::BUY, 300, 1));
    for (OrderId id = 34; id <= 64; ++id) {
        set.execute(msft, add(id, Side::BUY, 300 + id, 1));
    }
    expect(set.book(msft).activeOrderCount() == 64, "a book past its capacity grows and never refuses");
    expect(set.book(msft).capacityGrowths() != 0, "and the growth is counted");
}

void testASetStaysWhereItWasBuilt() {
    static_assert(!std::is_copy_constructible_v<BookSet> && !std::is_move_constructible_v<BookSet>,
                  "a set's owner is the thread that built it");
    static_assert(!std::is_copy_assignable_v<BookSet> && !std::is_move_assignable_v<BookSet>,
                  "a set's owner is the thread that built it");
    static_assert(std::is_same_v<decltype(std::declval<const BookSet&>().book(0)), const OrderBook&>,
                  "book gives read-only access");
}

}  // namespace

int main() {
    testHoldsItsShardsBooks();
    testExecuteRunsTheSlotsBook();
    testTheOutOverloadMatchesExecute();
    testBooksAreWarmedToCapacity();
    testASetStaysWhereItWasBuilt();
    std::cout << "book set tests passed" << std::endl;
    return EXIT_SUCCESS;
}
