#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#define private public
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#undef private

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

template <typename Callable>
void expectThrows(Callable&& callable, const std::string& message) {
    try {
        callable();
    } catch (const std::exception&) {
        return;
    }

    std::cerr << "FAILED: " << message << std::endl;
    std::exit(EXIT_FAILURE);
}

}  // namespace

int main() {
    const order_book::OrderBook book{};
    (void)book;
    std::cout << "order_book smoke test passed" << std::endl;

    using order_book::FillStatus;
    using order_book::Order;
    using order_book::OrderBook;
    using order_book::OrderLocation;
    using order_book::PriceLevel;
    using order_book::Side;

    OrderBook bookWithLevels{};
    expect(!bookWithLevels.hasBids(), "empty order book should not report bids");
    expect(!bookWithLevels.hasAsks(), "empty order book should not report asks");

    Order bidOrder(1, 100, 10, 3, Side::BUY);
    Order askOrder(2, 101, 5, 4, Side::SELL);
    bookWithLevels.bids.emplace(100, PriceLevel(100, Side::BUY, bidOrder));
    bookWithLevels.asks.emplace(101, PriceLevel(101, Side::SELL, askOrder));

    expect(bookWithLevels.hasBids(), "book should report the presence of bids");
    expect(bookWithLevels.hasAsks(), "book should report the presence of asks");
    expect(bookWithLevels.getBestBid().getOrderId() == 1, "best bid should return the earliest order from the best bid level");
    expect(bookWithLevels.getBestAsk().getOrderId() == 2, "best ask should return the earliest order from the best ask level");

    OrderBook snapshotBook{};
    snapshotBook.addOrder(Order(20, 105, 4, 1, Side::BUY));
    snapshotBook.addOrder(Order(21, 104, 3, 2, Side::BUY));
    snapshotBook.addOrder(Order(22, 104, 2, 3, Side::BUY));
    snapshotBook.addOrder(Order(23, 103, 1, 4, Side::BUY));
    snapshotBook.addOrder(Order(24, 101, 6, 5, Side::SELL));
    snapshotBook.addOrder(Order(25, 102, 5, 6, Side::SELL));
    snapshotBook.addOrder(Order(26, 102, 4, 7, Side::SELL));
    snapshotBook.addOrder(Order(27, 103, 2, 8, Side::SELL));

    const auto snapshot = snapshotBook.snapshot(2);
    expect(snapshot.bids.size() == 2, "snapshot should include only the requested number of bid levels");
    expect(snapshot.bids[0].price == 105, "snapshot bids should be ordered from highest price");
    expect(snapshot.bids[0].totalQuantity == 4, "snapshot should report the bid level aggregate quantity");
    expect(snapshot.bids[0].orderQuantity == 1, "snapshot should report the bid level order count");
    expect(snapshot.bids[1].price == 104, "snapshot should include the second-best bid level");
    expect(snapshot.bids[1].totalQuantity == 5, "snapshot should aggregate quantities at one bid price");
    expect(snapshot.bids[1].orderQuantity == 2, "snapshot should count orders at one bid price");

    expect(snapshot.asks.size() == 2, "snapshot should include only the requested number of ask levels");
    expect(snapshot.asks[0].price == 101, "snapshot asks should be ordered from lowest price");
    expect(snapshot.asks[0].totalQuantity == 6, "snapshot should report the ask level aggregate quantity");
    expect(snapshot.asks[0].orderQuantity == 1, "snapshot should report the ask level order count");
    expect(snapshot.asks[1].price == 102, "snapshot should include the second-best ask level");
    expect(snapshot.asks[1].totalQuantity == 9, "snapshot should aggregate quantities at one ask price");
    expect(snapshot.asks[1].orderQuantity == 2, "snapshot should count orders at one ask price");
    expect(snapshot.totalActiveVisibleOrders == 6,
           "snapshot should count orders only in the visible bid and ask levels");

    const auto defaultSnapshot = snapshotBook.snapshot();
    expect(defaultSnapshot.bids.size() == 3, "snapshot should use its default depth when none is provided");
    expect(defaultSnapshot.asks.size() == 3, "snapshot should use its default depth for asks when none is provided");

        OrderBook orderBook{};
        orderBook.addOrder(Order(10, 200, 4, 1, Side::BUY));
        orderBook.addOrder(Order(11, 200, 6, 2, Side::BUY));
        orderBook.addOrder(Order(12, 201, 3, 3, Side::SELL));

        OrderLocation bidLocation = orderBook.findActiveOrder(10);
        expect(bidLocation.tickPrice == 200, "active bid lookup should return the order price");
        expect(bidLocation.side == Side::BUY, "active bid lookup should return the order side");
        expect(bidLocation.itr->getOrderId() == 10, "active bid lookup should return the matching order iterator");

        OrderLocation askLocation = orderBook.findActiveOrder(12);
        expect(askLocation.tickPrice == 201, "active ask lookup should return the order price");
        expect(askLocation.side == Side::SELL, "active ask lookup should return the order side");
        expect(askLocation.itr->getOrderId() == 12, "active ask lookup should return the matching order iterator");

        expectThrows([&orderBook] { orderBook.findActiveOrder(99); },
               "active order lookup should reject an unknown order ID");

        orderBook.removeOrder(10);
        expectThrows([&orderBook] { orderBook.findActiveOrder(10); },
               "removed order should no longer be indexed");
        expect(orderBook.getBestBid().getOrderId() == 11,
            "removing one order should preserve the next order at the price level");
        expect(orderBook.hasBids(), "a price level should remain while it still contains an order");

        orderBook.removeOrder(11);
        expect(!orderBook.hasBids(), "removing the last bid should remove the bid price level");

        orderBook.removeOrder(12);
        expect(!orderBook.hasAsks(), "removing the last ask should remove the ask price level");
        expectThrows([&orderBook] { orderBook.removeOrder(99); },
               "removing an unknown order ID should throw");

        OrderBook consumeBook{};
        consumeBook.addOrder(Order(30, 300, 5, 1, Side::SELL));
        consumeBook.addOrder(Order(31, 300, 5, 2, Side::SELL));

        const auto consumed = consumeBook.consumeLevel(Side::SELL, 300, 7);
        expect(consumed.unfilledQuantity == 0, "consuming within level quantity should leave nothing unfilled");
        expectThrows([&consumeBook] { consumeBook.findActiveOrder(30); },
               "a fully filled order should no longer be indexed");
        expectThrows([&consumeBook] { consumeBook.removeOrder(30); },
               "cancelling a fully filled order should throw instead of using a stale iterator");
        expect(consumeBook.findActiveOrder(31).itr->getRemainingQuantity() == 3,
            "a partially filled order should stay indexed with its reduced quantity");
        expect(consumeBook.getBestAsk().getOrderId() == 31, "the partially filled order should remain at the level");

        const auto drained = consumeBook.consumeLevel(Side::SELL, 300, 10);
        expect(drained.unfilledQuantity == 7, "consuming past the level should report the unfilled remainder");
        expect(!consumeBook.hasAsks(), "a fully consumed level should be removed from the book");
        expectThrows([&consumeBook] { consumeBook.findActiveOrder(31); },
               "the last filled order should no longer be indexed");
        expectThrows([&consumeBook] { consumeBook.consumeLevel(Side::SELL, 300, 1); },
               "consuming from a missing price level should throw");

        consumeBook.addOrder(Order(32, 299, 2, 3, Side::BUY));
        consumeBook.consumeLevel(Side::BUY, 299, 2);
        expect(!consumeBook.hasBids(), "a fully consumed bid level should be removed from the book");
        expectThrows([&consumeBook] { consumeBook.findActiveOrder(32); },
               "a fully filled bid should no longer be indexed");

    OrderBook matchBook{};
    matchBook.addOrder(Order(40, 101, 3, 1, Side::SELL));
    matchBook.addOrder(Order(41, 101, 4, 2, Side::SELL));
    matchBook.addOrder(Order(42, 102, 5, 3, Side::SELL));
    matchBook.addOrder(Order(43, 104, 5, 4, Side::SELL));

    const auto buyTrades = matchBook.submitOrder(Order(50, 102, 10, 5, Side::BUY));
    expect(buyTrades.size() == 3, "a crossing buy should trade against each resting order it fills");
    expect(buyTrades[0].getRestingId() == 40 && buyTrades[1].getRestingId() == 41,
           "orders at the same price should match in arrival order");
    expect(buyTrades[2].getRestingId() == 42, "matching should move to the next ask level once the best is exhausted");
    expect(buyTrades[0].getExecutionPrice() == 101 && buyTrades[2].getExecutionPrice() == 102,
           "trades should execute at the resting order's price");
    expect(buyTrades[2].getExecutionQuantity() == 3, "the last trade should fill only the incoming remainder");
    expect(buyTrades[0].getIncomingId() == 50 && buyTrades[0].getIncomingSide() == Side::BUY,
           "trades should record the incoming order and side");
    expect(buyTrades[0].getTradeId() == 1 && buyTrades[2].getTradeId() == 3, "trade IDs should increase from one");
    expectThrows([&matchBook] { matchBook.findActiveOrder(50); },
           "a fully filled incoming order should never be indexed");
    expect(!matchBook.hasBids(), "a fully filled incoming order should not rest");
    expect(matchBook.getBestAsk().getOrderId() == 42 && matchBook.getBestAsk().getRemainingQuantity() == 2,
           "a partially filled resting ask should keep its place with reduced quantity");

    const auto restingBuyTrades = matchBook.submitOrder(Order(51, 103, 6, 6, Side::BUY));
    expect(restingBuyTrades.size() == 1, "a buy should stop matching when the best ask no longer crosses");
    expect(restingBuyTrades[0].getExecutionQuantity() == 2, "the buy should fill what crosses before resting");
    expect(matchBook.getBestBid().getOrderId() == 51 && matchBook.getBestBid().getRemainingQuantity() == 4,
           "the unfilled remainder should rest at its limit price");
    expect(matchBook.findActiveOrder(51).tickPrice == 103, "a resting remainder should be indexed");
    expect(matchBook.getBestBid().getExecutionLevel() == FillStatus::PARTIAL,
           "a resting remainder should be marked partially filled");
    expect(matchBook.getBestAsk().getOrderId() == 43, "a non-crossing ask should be left untouched");

    const auto passiveTrades = matchBook.submitOrder(Order(52, 104, 1, 7, Side::SELL));
    expect(passiveTrades.empty(), "a sell above the best bid should not trade");
    expect(matchBook.findActiveOrder(52).itr->getRemainingQuantity() == 1, "a non-crossing sell should rest in full");
    expect(matchBook.getBestAsk().getOrderId() == 43, "a new order at an existing price should queue behind older orders");

    matchBook.addOrder(Order(53, 103, 2, 8, Side::BUY));
    matchBook.addOrder(Order(54, 102, 5, 9, Side::BUY));
    const auto sellTrades = matchBook.submitOrder(Order(55, 102, 8, 10, Side::SELL));
    expect(sellTrades.size() == 3, "a crossing sell should trade down through the bid levels");
    expect(sellTrades[0].getRestingId() == 51 && sellTrades[1].getRestingId() == 53,
           "a sell should match the highest bid first, then FIFO within the level");
    expect(sellTrades[2].getRestingId() == 54 && sellTrades[2].getExecutionPrice() == 102,
           "a sell priced equal to the best bid should cross");
    expect(sellTrades[2].getExecutionQuantity() == 2, "the sell should take only its remaining quantity");
    expect(sellTrades[0].getTradeId() == 5, "trade IDs should keep increasing across submissions");
    expectThrows([&matchBook] { matchBook.findActiveOrder(51); }, "fully filled resting bids should be unindexed");
    expect(matchBook.getBestBid().getOrderId() == 54 && matchBook.getBestBid().getRemainingQuantity() == 3,
           "the partially filled bid should remain best");

    OrderBook emptyBook{};
    expect(emptyBook.submitOrder(Order(60, 100, 5, 1, Side::SELL)).empty(),
           "an order into an empty opposing side should not trade");
    expect(emptyBook.hasAsks() && emptyBook.findActiveOrder(60).side == Side::SELL,
           "an order into an empty opposing side should rest");

    Order baseOrder(1, 100, 10, 3, Side::BUY);
    expect(baseOrder.getOrderId() == 1, "order ID should be preserved");
    expect(baseOrder.getSequenceNum() == 3, "sequence number should be preserved");
    expect(baseOrder.getPriceTick() == 100, "price tick should be preserved");
    expect(baseOrder.getInitialQuantity() == 10, "initial quantity should be preserved");
    expect(baseOrder.getRemainingQuantity() == 10, "remaining quantity should start at the initial quantity");
    expect(baseOrder.getSide() == Side::BUY, "buy side should be preserved");
    expect(baseOrder.getExecutionLevel() == FillStatus::NONE, "new order should start with no execution lifecycle");
    expect(baseOrder.orderIsActive(), "new order should be active");

    expect(baseOrder.fillQuantity(3) == 0, "partial fill should not return excess quantity");
    expect(baseOrder.getRemainingQuantity() == 7, "remaining quantity should decrease after a partial fill");
    expect(baseOrder.getExecutionLevel() == FillStatus::PARTIAL, "partial fill should set the execution lifecycle to PARTIAL");
    expect(baseOrder.orderIsActive(), "partially filled order should stay active");

    expect(baseOrder.fillQuantity(7) == 0, "full fill should not return excess quantity");
    expect(baseOrder.getRemainingQuantity() == 0, "remaining quantity should reach zero after a full fill");
    expect(baseOrder.getExecutionLevel() == FillStatus::FILLED, "full fill should set the execution lifecycle to FILLED");
    expect(!baseOrder.orderIsActive(), "fully filled order should no longer be active");

    Order sequenceOrder(2, 100, 5, 1, Side::SELL);
    sequenceOrder.decrementSequence();
    expect(sequenceOrder.getSequenceNum() == 0, "sequence number should decrease as expected");
    expectThrows([&sequenceOrder] { sequenceOrder.decrementSequence(); }, "sequence number should not decrease below zero");

    expectThrows([] { Order invalidId(0, 100, 5, 1, Side::BUY); }, "constructor should reject a non-positive ID");
    expectThrows([] { Order invalidTick(1, 0, 5, 1, Side::BUY); }, "constructor should reject a non-positive tick");
    expectThrows([] { Order invalidQuantity(1, 100, 0, 1, Side::BUY); }, "constructor should reject a non-positive quantity");
    expectThrows([] { Order invalidArrival(1, 100, 5, 0, Side::BUY); }, "constructor should reject a non-positive sequence number");
    expectThrows([&baseOrder] { baseOrder.fillQuantity(0); }, "fillQuantity should reject zero fill requests");

    std::cout << "order tests passed" << std::endl;
    return EXIT_SUCCESS;
}
