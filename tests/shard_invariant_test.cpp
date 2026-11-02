#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "order_book/order_book.hpp"
#include "order_book/runtime/shard.hpp"

namespace order_book {

// Befriended by OrderBook and Shard so this test can break them on purpose.
struct OrderBookTestAccess {
    static std::uint64_t& aggregate(OrderBook& book, Price askPrice) { return book.asks.at(askPrice).quantity; }
    static void setNextPosition(runtime::Shard& shard, std::uint64_t position) { shard.nextLogPosition = position; }
    static void setDirectory(runtime::Shard& shard, const runtime::InstrumentDirectory& directory) { shard.instruments = &directory; }
    static OrderBook& book(runtime::Shard& shard, std::uint32_t slot) { return const_cast<OrderBook&>(shard.bookSet.book(slot)); }
};

}  // namespace order_book

namespace {

using namespace order_book;
using namespace order_book::runtime;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

Instrument on(std::string_view text, std::uint32_t shard) {
    return Instrument{*Symbol::parse(text), shard, std::nullopt, std::nullopt, std::nullopt};
}

replay::AddCommand sell(OrderId id, Price price, Quantity quantity) {
    replay::AddCommand command{};
    command.orderId = id;
    command.side = Side::SELL;
    command.price = price;
    command.quantity = quantity;
    return command;
}

void expectViolation(const Shard& shard, ShardViolationKind kind, std::uint32_t slot, const std::string& what) {
    const std::optional<ShardViolation> violation = shard.checkInvariants();
    expect(violation.has_value(), what + " is found");
    expect(violation->kind == kind && violation->slot == slot, what + " is named");
    expect(!violation->message.empty(), what + " is described");
}

void testABrokenBookIsFound() {
    const InstrumentDirectory directory({on("AAPL", 0), on("MSFT", 0)}, 1);
    Shard shard(directory, 0);
    shard.process(replay::Command{sell(1, 100, 5)}, 1);
    expect(!shard.checkInvariants(), "a sound shard passes");

    OrderBookTestAccess::aggregate(OrderBookTestAccess::book(shard, 1), 100) = 6;
    expectViolation(shard, ShardViolationKind::BOOK, 1, "a book that fails its own check");
}

void testASlotThatDoesNotMapBackIsFound() {
    const InstrumentDirectory directory({on("AAPL", 0), on("MSFT", 0)}, 1);
    Shard shard(directory, 0);

    const InstrumentDirectory moved({on("AAPL", 0), on("MSFT", 1)}, 2);
    OrderBookTestAccess::setDirectory(shard, moved);
    expectViolation(shard, ShardViolationKind::SLOT_MAPPING, 1, "a book whose instrument moved to another shard");

    const InstrumentDirectory renumbered({on("AAPL", 0), on("AMD", 0), on("MSFT", 0)}, 1);
    OrderBookTestAccess::setDirectory(shard, renumbered);
    expectViolation(shard, ShardViolationKind::SLOT_MAPPING, 1, "a book whose instrument moved to another slot");

    const InstrumentDirectory gone({on("AAPL", 0)}, 1);
    OrderBookTestAccess::setDirectory(shard, gone);
    expectViolation(shard, ShardViolationKind::SLOT_MAPPING, 1, "a book the directory no longer lists");

    OrderBookTestAccess::setDirectory(shard, directory);
    expect(!shard.checkInvariants(), "and the true directory still passes");
}

void testALogPositionOutOfStepIsFound() {
    const InstrumentDirectory directory({on("AAPL", 0)}, 1);
    Shard shard(directory, 0, defaultBookCapacity, 1000);
    shard.process(replay::Command{sell(1, 100, 5)}, 0);
    shard.process(replay::Command{replay::CancelCommand{1}}, 0);
    expect(!shard.checkInvariants(), "two commands from position 1000 stand at 1002");

    OrderBookTestAccess::setNextPosition(shard, 1003);
    expectViolation(shard, ShardViolationKind::LOG_POSITION, 0, "a next position past the commands taken");
    OrderBookTestAccess::setNextPosition(shard, 1001);
    expectViolation(shard, ShardViolationKind::LOG_POSITION, 0, "a next position behind them");
}

}  // namespace

int main() {
    testABrokenBookIsFound();
    testASlotThatDoesNotMapBackIsFound();
    testALogPositionOutOfStepIsFound();
    std::cout << "shard invariant tests passed" << std::endl;
    return EXIT_SUCCESS;
}
