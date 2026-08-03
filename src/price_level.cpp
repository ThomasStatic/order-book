#include <algorithm>

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

    OrderLocation PriceLevel::addOrder(Order newOrder) {
        std::list<Order>::iterator itr = orders.insert(orders.end(), newOrder);
        quantity += newOrder.getRemainingQuantity();
        
        OrderLocation location(tickPrice, side, itr);
        return location;
    }

    const Order& PriceLevel::getOldestOrder() const{
        return orders.front();
    }

    void PriceLevel::fillOldestOrder(unsigned int fillQuant) {
        Order& oldestOrder = orders.front();

        const unsigned int availableQuant = oldestOrder.getRemainingQuantity();
        const unsigned int fillAmount = std::min(fillQuant, availableQuant);
        oldestOrder.fillQuantity(fillAmount);
        quantity -= fillAmount;

        if (oldestOrder.getRemainingQuantity() == 0) {
            orders.pop_front();
        }
    }

    QuantityConsumptionStatus PriceLevel::consumeQuantity(unsigned int fillQuant) {
        if(fillQuant == 0) {
            return QuantityConsumptionStatus::INVALID_REQUEST;
        }

        while (fillQuant > 0 && quantity > 0) {
            unsigned int oldQuant = quantity;
            fillOldestOrder(fillQuant);
            fillQuant = fillQuant - (oldQuant - quantity);
        }

        if(quantity > 0) {
            return QuantityConsumptionStatus::LEVEL_EXHAUSTED;
        }

        if(quantity == 0) {
            return QuantityConsumptionStatus::SATISFIED;
        }

        return QuantityConsumptionStatus::INVALID_REQUEST;
    }

    void PriceLevel::removeOrder(std::list<Order>::iterator itr) {
        Order targetOrder = *itr;
        quantity -= targetOrder.getRemainingQuantity();
        orders.erase(itr);
    }

    bool PriceLevel::isEmpty() const{
        return orders.empty();
    }
}