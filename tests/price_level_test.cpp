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
       expect(level.getAggregateQuantity() == 8, "price level should report aggregate remaining quantity");
       expect(level.getOrdersQuantity() == 2, "price level should report the number of active orders");
    expect(level.consumeQuantity(5).status == QuantityConsumptionStatus::LEVEL_EXHAUSTED,
           "consuming the full quantity of the oldest order should leave the next order behind");
       expect(level.getAggregateQuantity() == 3, "price level aggregate should decrease after consumption");
       expect(level.getOrdersQuantity() == 1, "price level order count should exclude fully consumed orders");
    expect(level.getOldestOrder().getOrderId() == 2,
           "FIFO should advance to the next order once the oldest one is fully consumed");
    expect(level.getOldestOrder().getRemainingQuantity() == 3,
           "the next order should still carry its remaining quantity after FIFO advancement");

    expect(level.consumeQuantity(3).status == QuantityConsumptionStatus::SATISFIED,
           "consuming the remaining quantity should fully satisfy the level");
    expect(level.isEmpty(), "the level should become empty once all quantity has been consumed");
    expect(level.getAggregateQuantity() == 0, "an empty price level should report zero aggregate quantity");
    expect(level.getOrdersQuantity() == 0, "an empty price level should report zero active orders");

    PriceLevel aggregateLevel(100, Side::BUY, Order(3, 100, 4, 3, Side::BUY));
    aggregateLevel.addOrder(Order(4, 100, 6, 4, Side::BUY));

    expect(aggregateLevel.consumeQuantity(5).status == QuantityConsumptionStatus::LEVEL_EXHAUSTED,
           "aggregate quantity should be tracked across multiple orders");
    expect(aggregateLevel.consumeQuantity(5).status == QuantityConsumptionStatus::SATISFIED,
           "a second consumption pass should satisfy the remaining aggregate quantity");
    expect(aggregateLevel.isEmpty(), "the aggregate level should be empty after all quantity is consumed");

    PriceLevel emptyLevel(100, Side::BUY, Order(5, 100, 2, 5, Side::BUY));
    expect(emptyLevel.consumeQuantity(2).status == QuantityConsumptionStatus::SATISFIED,
           "consuming the last quantity should satisfy an otherwise empty level");
    expect(emptyLevel.isEmpty(), "an emptied level should report itself as empty");
    expect(emptyLevel.consumeQuantity(1).status == QuantityConsumptionStatus::SATISFIED,
           "consuming from an already empty level should remain a no-op success");

    PriceLevel fillLevel(100, Side::SELL, Order(6, 100, 4, 6, Side::SELL));
    fillLevel.addOrder(Order(7, 100, 6, 7, Side::SELL));

    const auto partial = fillLevel.consumeQuantity(7);
    expect(partial.unfilledQuantity == 0, "a level with enough quantity should fill the whole request");
    expect(partial.fills.size() == 2, "consumption should report one fill per touched order");
    expect(partial.fills[0].orderId == 6 && partial.fills[0].filledQuantity == 4 && partial.fills[0].fullyFilled,
           "the oldest order should be reported as fully filled");
    expect(partial.fills[1].orderId == 7 && partial.fills[1].filledQuantity == 3 && !partial.fills[1].fullyFilled,
           "the next order should be reported as partially filled");

    const auto overflow = fillLevel.consumeQuantity(10);
    expect(overflow.unfilledQuantity == 7, "consumption should report the quantity the level could not supply");
    expect(overflow.fills.size() == 1 && overflow.fills[0].filledQuantity == 3 && overflow.fills[0].fullyFilled,
           "the last order should be fully filled by an oversized request");

    const auto emptyResult = fillLevel.consumeQuantity(2);
    expect(emptyResult.unfilledQuantity == 2 && emptyResult.fills.empty(),
           "an empty level should fill nothing and report the full request as unfilled");

    const auto zeroResult = fillLevel.consumeQuantity(0);
    expect(zeroResult.status == QuantityConsumptionStatus::INVALID_REQUEST, "a zero-quantity request should be invalid");

    std::cout << "price level tests passed" << std::endl;
    return EXIT_SUCCESS;
}
