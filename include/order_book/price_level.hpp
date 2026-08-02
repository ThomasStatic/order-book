#pragma once

#include <list>
#include <stdexcept>
#include "order.hpp"
#include "side.hpp"

namespace order_book {

    enum class QuantityConsumtionStatus {
        SATISFIED,
        LEVEL_EXHAUSTED,
        INVALID_REQUEST
    };

    class PriceLevel {
    private:
        const unsigned int tickPrice;
        const Side side;
        
        std::list<Order> orders;
        unsigned int quantity; // aggregate of all orders

        void fillOldestOrder(unsigned int fillQuant);

    public:
        PriceLevel(unsigned int price, Side s, Order firstOrder);

        void addOrder(Order newOrder);
        Order getOldestOrder() const;

        QuantityConsumtionStatus consumeQuantity(unsigned int fillQuant);


    };
}