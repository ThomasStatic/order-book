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
