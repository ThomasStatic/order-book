#pragma once

#include <cstdint>
#include <list>
#include <stdexcept>
#include <iterator>
#include <vector>

#include "order.hpp"
#include "side.hpp"
#include "order_location.hpp"

namespace order_book {

    enum class QuantityConsumptionStatus {
        SATISFIED,
        LEVEL_EXHAUSTED,
        INVALID_REQUEST
    };

    struct OrderFill {
        unsigned int orderId;
        unsigned int filledQuantity;
        bool fullyFilled; // order was removed from the level
    };

    struct ConsumptionResult {
        QuantityConsumptionStatus status;
        unsigned int unfilledQuantity; // requested quantity the level could not supply
        std::vector<OrderFill> fills;  // in FIFO order
    };

    class PriceLevel {
    private:
        // OrderBook alone may fill a single order, since it must also unindex the order
        // and erase the level once empty.
        friend class OrderBook;
        // Test-only access to internals; defined in tests, never in the library.
        friend struct OrderBookTestAccess;

        const unsigned int tickPrice;
        const Side side;
        
        std::list<Order> orders;
        // Aggregate of all orders' remaining quantities. Wider than a Quantity, so it wraps only
        // past about 2^32 orders of the maximum quantity at one price.
        std::uint64_t quantity = 0;

        // Fills up to fillQuant from the oldest order only, popping it if fully filled.
        OrderFill fillOldestOrder(unsigned int fillQuant);

    public:
        PriceLevel(unsigned int price, Side s);
        PriceLevel(unsigned int price, Side s, Order initialOrder);

        OrderLocation addOrder(Order newOrder);
        const Order& getOldestOrder() const;

        ConsumptionResult consumeQuantity(unsigned int fillQuant);

        void removeOrder(std::list<Order>::iterator itr);

        bool isEmpty() const;

        std::uint64_t getAggregateQuantity() const;
        unsigned int getOrdersQuantity() const;


    };
}