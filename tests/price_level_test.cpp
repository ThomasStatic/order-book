#include <cstdlib>
#include <iostream>
#include <string>

#include "order_book/order.hpp"
#include "order_book/price_level.hpp"

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

}  // namespace

int main() {
    using order_book::Order;
    using order_book::PriceLevel;
    using order_book::QuantityConsumptionStatus;
    using order_book::Side;

    Order first(1, 100, 5, 1, Side::BUY);
    Order second(2, 100, 3, 2, Side::BUY);

    PriceLevel level(100, Side::BUY, first);
    level.addOrder(second);

    expect(!level.isEmpty(), "newly initialized price level should not be empty");
    expect(level.consumeQuantity(5) == QuantityConsumptionStatus::LEVEL_EXHAUSTED,
           "consuming the full quantity of the oldest order should leave the next order behind");
    expect(level.getOldestOrder().getOrderId() == 2,
           "FIFO should advance to the next order once the oldest one is fully consumed");
    expect(level.getOldestOrder().getRemainingQuantity() == 3,
           "the next order should still carry its remaining quantity after FIFO advancement");

    expect(level.consumeQuantity(3) == QuantityConsumptionStatus::SATISFIED,
           "consuming the remaining quantity should fully satisfy the level");
    expect(level.isEmpty(), "the level should become empty once all quantity has been consumed");

    PriceLevel aggregateLevel(100, Side::BUY, Order(3, 100, 4, 3, Side::BUY));
    aggregateLevel.addOrder(Order(4, 100, 6, 4, Side::BUY));

    expect(aggregateLevel.consumeQuantity(5) == QuantityConsumptionStatus::LEVEL_EXHAUSTED,
           "aggregate quantity should be tracked across multiple orders");
    expect(aggregateLevel.consumeQuantity(5) == QuantityConsumptionStatus::SATISFIED,
           "a second consumption pass should satisfy the remaining aggregate quantity");
    expect(aggregateLevel.isEmpty(), "the aggregate level should be empty after all quantity is consumed");

    PriceLevel emptyLevel(100, Side::BUY, Order(5, 100, 2, 5, Side::BUY));
    expect(emptyLevel.consumeQuantity(2) == QuantityConsumptionStatus::SATISFIED,
           "consuming the last quantity should satisfy an otherwise empty level");
    expect(emptyLevel.isEmpty(), "an emptied level should report itself as empty");
    expect(emptyLevel.consumeQuantity(1) == QuantityConsumptionStatus::SATISFIED,
           "consuming from an already empty level should remain a no-op success");

    std::cout << "price level tests passed" << std::endl;
    return EXIT_SUCCESS;
}
