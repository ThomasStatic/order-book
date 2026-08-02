#include "order_book/price_level.hpp"

namespace order_book {
    PriceLevel::PriceLevel(unsigned int price, Side s, Order firstOrder): tickPrice(price), side(s) {
        if (price == 0 ) {
            throw std::invalid_argument("Non-positive price error");
        }

        if(firstOrder.getPriceTick() != price) {
            throw std::invalid_argument("Order doesn't match supplied price error");
        }

        orders.push_back(firstOrder);
        quantity = firstOrder.getRemainingQuantity();
    }

    void PriceLevel::addOrder(Order newOrder) {
        orders.push_back(newOrder);
        quantity += newOrder.getRemainingQuantity();
    }

    Order PriceLevel::getOldestOrder() const{
        return orders.front();
    }

    void PriceLevel::fillOldestOrder(unsigned int fillQuant) {
        Order oldestOrder = getOldestOrder();
        
        int remainingQuant = oldestOrder.fillQuantity(fillQuant);
        quantity = quantity - (fillQuant - remainingQuant);
        
        if(fillQuant == 0) {
            orders.pop_front();
        }
    }

    QuantityConsumtionStatus PriceLevel::consumeQuantity(unsigned int fillQuant) {
        while (fillQuant > 0 && quantity > 0) {
            unsigned int oldQuant = quantity;
            fillOldestOrder(fillQuant);
            fillQuant = fillQuant - (oldQuant - quantity);
        }

        if(quantity > 0) {
            return QuantityConsumtionStatus::LEVEL_EXHAUSTED;
        }

        if(quantity == 0) {
            return QuantityConsumtionStatus::SATISFIED;
        }

        return QuantityConsumtionStatus::INVALID_REQUEST;
    }
}