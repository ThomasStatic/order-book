#pragma once

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
        const unsigned int tickPrice;
        const Side side;
        
        std::list<Order> orders;
        unsigned int quantity = 0; // aggregate of all orders

        OrderFill fillOldestOrder(unsigned int fillQuant);

    public:
        PriceLevel(unsigned int price, Side s);
        PriceLevel(unsigned int price, Side s, Order initialOrder);

        OrderLocation addOrder(Order newOrder);
        const Order& getOldestOrder() const;

        ConsumptionResult consumeQuantity(unsigned int fillQuant);

        void removeOrder(std::list<Order>::iterator itr);

        bool isEmpty() const;

        unsigned int getAggregateQuantity() const;
        unsigned int getOrdersQuantity() const;


    };
}