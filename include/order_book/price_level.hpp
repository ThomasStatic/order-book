#pragma once

#include <list>
#include <stdexcept>
#include <iterator>

#include "order.hpp"
#include "side.hpp"
#include "order_location.hpp"

namespace order_book {

    enum class QuantityConsumptionStatus {
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
        PriceLevel(unsigned int price, Side s);

        OrderLocation addOrder(Order newOrder);
        const Order& getOldestOrder() const;

        QuantityConsumptionStatus consumeQuantity(unsigned int fillQuant);

        void removeOrder(std::list<Order>::iterator itr);

        bool isEmpty() const;


    };
}