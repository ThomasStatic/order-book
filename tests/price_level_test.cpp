#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "order_book/order.hpp"
#include "order_book/price_level.hpp"

namespace order_book {

// Befriended by PriceLevel so this test can fill a level directly, as OrderBook does.
struct OrderBookTestAccess {
    static OrderFill fillOldestOrder(PriceLevel& level, Quantity quantity) { return level.fillOldestOrder(quantity); }
};

}  // namespace order_book

namespace {

using Access = order_book::OrderBookTestAccess;

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
    using order_book::Order;
    using order_book::OrderFill;
    using order_book::PriceLevel;
    using order_book::Side;

    PriceLevel level(100, Side::BUY, Order(1, 100, 5, 1, Side::BUY));
    level.addOrder(Order(2, 100, 3, 2, Side::BUY));

    expect(!level.isEmpty(), "newly initialized price level should not be empty");
    expect(level.getAggregateQuantity() == 8, "price level should report aggregate remaining quantity");
    expect(level.getOrderCount() == 2, "price level should report the number of active orders");
    expect(level.getOldestOrder().getOrderId() == 1, "the first order added should be the oldest");

    const OrderFill partial = Access::fillOldestOrder(level, 2);
    expect(partial.orderId == 1 && partial.filledQuantity == 2 && !partial.fullyFilled,
           "a smaller fill should partially fill the oldest order");
    expect(level.getAggregateQuantity() == 6, "price level aggregate should decrease after a partial fill");
    expect(level.getOrderCount() == 2, "a partially filled order should stay in the level");
    expect(level.getOldestOrder().getRemainingQuantity() == 3, "the oldest order should keep what it has left");

    const OrderFill oversized = Access::fillOldestOrder(level, 10);
    expect(oversized.orderId == 1 && oversized.filledQuantity == 3 && oversized.fullyFilled,
           "a fill larger than the oldest order should fill only that order");
    expect(level.getAggregateQuantity() == 3, "price level aggregate should exclude a fully filled order");
    expect(level.getOrderCount() == 1, "price level order count should exclude fully filled orders");
    expect(level.getOldestOrder().getOrderId() == 2,
           "FIFO should advance to the next order once the oldest one is fully filled");
    expect(level.getOldestOrder().getRemainingQuantity() == 3,
           "the next order should still carry its remaining quantity after FIFO advancement");

    const OrderFill last = Access::fillOldestOrder(level, 3);
    expect(last.orderId == 2 && last.filledQuantity == 3 && last.fullyFilled,
           "an exact fill should fully fill the oldest order");
    expect(level.isEmpty(), "the level should become empty once every order is filled");
    expect(level.getAggregateQuantity() == 0, "an empty price level should report zero aggregate quantity");
    expect(level.getOrderCount() == 0, "an empty price level should report zero active orders");
    expectThrows([&level] { Access::fillOldestOrder(level, 1); }, "filling an empty level should throw");

    PriceLevel removalLevel(100, Side::SELL);
    const order_book::OrderLocation oldest = removalLevel.addOrder(Order(3, 100, 4, 3, Side::SELL));
    const order_book::OrderLocation middle = removalLevel.addOrder(Order(4, 100, 6, 4, Side::SELL));
    removalLevel.addOrder(Order(5, 100, 2, 5, Side::SELL));
    expect(middle.tickPrice == 100 && middle.side == Side::SELL && middle.level == &removalLevel &&
               middle.itr->getOrderId() == 4,
           "addOrder should return the order's price, side, level, and list position");
    expectThrows([&removalLevel] { Access::fillOldestOrder(removalLevel, 0); },
                 "filling zero quantity from a level should throw");
    expect(removalLevel.getAggregateQuantity() == 12 && removalLevel.getOldestOrder().getRemainingQuantity() == 4,
           "a refused fill should leave the level unchanged");

    Access::fillOldestOrder(removalLevel, 1);
    removalLevel.removeOrder(oldest.itr);
    expect(removalLevel.getAggregateQuantity() == 8 && removalLevel.getOrderCount() == 2,
           "removing a partially filled order should take only its remaining quantity off the aggregate");
    expect(removalLevel.getOldestOrder().getOrderId() == 4, "removing the oldest order should advance FIFO");

    removalLevel.removeOrder(middle.itr);
    expect(removalLevel.getAggregateQuantity() == 2 && removalLevel.getOrderCount() == 1 &&
               removalLevel.getOldestOrder().getOrderId() == 5,
           "removing an order should leave the orders after it in place");

    std::cout << "price level tests passed" << std::endl;
    return EXIT_SUCCESS;
}
