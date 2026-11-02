#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "order_book/runtime/shard.hpp"

namespace {

using namespace order_book;
using namespace order_book::runtime;
using replay::AddCommand;
using replay::CancelCommand;
using replay::Command;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Instrument on(std::string_view text, std::uint32_t shard) {
    return Instrument{*Symbol::parse(text), shard, std::nullopt, std::nullopt, std::nullopt};
}

Command add(OrderId id, Side side, Price price, Quantity quantity) {
    AddCommand command{};
    command.orderId = id;
    command.side = side;
    command.price = price;
    command.quantity = quantity;
    return command;
}

void testProcessRunsTheSlotsBook() {
    const InstrumentDirectory directory({on("AAPL", 0), on("MSFT", 0), on("GOOG", 1)}, 2);
    Shard shard(directory, 0);
    expect(shard.size() == 2, "a shard holds its instruments' books");

    const std::uint32_t aapl = directory.find(*Symbol::parse("AAPL"))->slot;
    const std::uint32_t msft = directory.find(*Symbol::parse("MSFT"))->slot;
    shard.process(add(1, Side::SELL, 100, 5), aapl);
    expect(shard.result().events().size() == 1 && std::holds_alternative<OrderAccepted>(shard.result().events()[0]),
           "the result holds the command's events");
    shard.process(add(1, Side::BUY, 100, 5), msft);
    expect(shard.books().book(aapl).activeOrderCount() == 1 && shard.books().book(msft).activeOrderCount() == 1,
           "the slot chooses the book, so the same ID on two books doesn't trade");

    shard.process(add(2, Side::BUY, 100, 2), aapl);
    expect(shard.result().events().size() == 2 && std::holds_alternative<TradeExecuted>(shard.result().events()[1]),
           "a later command overwrites the result");
    shard.process(CancelCommand{77}, msft);
    expect(shard.result().events().size() == 1 && std::holds_alternative<CancelRejected>(shard.result().events()[0]),
           "and a cancellation's events are its own");
}

void testPositionsCountCommands() {
    const InstrumentDirectory directory({on("AAPL", 0)}, 1);
    Shard shard(directory, 0, defaultBookCapacity, 1000);
    expect(shard.nextPosition() == 1000 && shard.commandsTaken() == 0, "a shard starts at its first position");
    shard.process(add(1, Side::SELL, 100, 5), 0);
    shard.process(CancelCommand{7}, 0);
    expect(shard.nextPosition() == 1002 && shard.commandsTaken() == 2, "a rejected command takes a position too");

    if constexpr (invariantChecksAfterCommands) {
        expect(shard.lastProgress() == ShardProgress{0, 1001}, "checked builds record where the last command stood");
    } else {
        expect(shard.lastProgress() == ShardProgress{}, "other builds pay nothing for it");
    }
}

void testASoundShardPassesItsCheck() {
    const InstrumentDirectory directory({on("AAPL", 0), on("MSFT", 0), on("GOOG", 1)}, 2);
    Shard first(directory, 0);
    const Shard second(directory, 1);
    first.process(add(1, Side::SELL, 100, 5), 0);
    expect(!first.checkInvariants(), "a shard with orders is sound");
    expect(!second.checkInvariants(), "and so is an idle one");
}

void testAResultHoldsALargeResponse() {
    const InstrumentDirectory directory({on("AAPL", 0)}, 1);
    Shard shard(directory, 0, BookCapacity{1024, 8, 8});
    for (OrderId id = 1; id <= 600; ++id) {
        shard.process(add(id, Side::SELL, 100, 1), 0);
    }
    shard.process(add(1000, Side::BUY, 100, 600), 0);
    expect(shard.result().events().size() == 601, "a response past what was reserved still comes back whole");
    expect(shard.result().submission().getGeneratedTrades().size() == 600, "with every trade");
}

}  // namespace

int main() {
    testProcessRunsTheSlotsBook();
    testPositionsCountCommands();
    testASoundShardPassesItsCheck();
    testAResultHoldsALargeResponse();
    std::cout << "shard tests passed" << std::endl;
    return EXIT_SUCCESS;
}
