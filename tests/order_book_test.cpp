#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include "order_book/order.hpp"
#include "order_book/order_book.hpp"

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
    using order_book::Side;

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
