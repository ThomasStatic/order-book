#include <algorithm>

#include "order_book/price_level.hpp"

namespace order_book {
    PriceLevel::PriceLevel(unsigned int price, Side s): tickPrice(price), side(s) {}

    PriceLevel::PriceLevel(unsigned int price, Side s, Order initialOrder): PriceLevel(price, s) {
        addOrder(initialOrder);
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

    OrderFill PriceLevel::fillOldestOrder(unsigned int fillQuant) {
        Order& oldestOrder = orders.front();

        const unsigned int availableQuant = oldestOrder.getRemainingQuantity();
        const unsigned int fillAmount = std::min(fillQuant, availableQuant);
        oldestOrder.fillQuantity(fillAmount);
        quantity -= fillAmount;

        OrderFill fill{oldestOrder.getOrderId(), fillAmount, false};
        if (oldestOrder.getRemainingQuantity() == 0) {
            fill.fullyFilled = true;
            orders.pop_front();
        }
        return fill;
    }

    ConsumptionResult PriceLevel::consumeQuantity(unsigned int fillQuant) {
        ConsumptionResult result{QuantityConsumptionStatus::INVALID_REQUEST, fillQuant, {}};
        if(fillQuant == 0) {
            return result;
        }

        while (result.unfilledQuantity > 0 && quantity > 0) {
            OrderFill fill = fillOldestOrder(result.unfilledQuantity);
            result.unfilledQuantity -= fill.filledQuantity;
            result.fills.push_back(fill);
        }

        result.status = quantity > 0 ? QuantityConsumptionStatus::LEVEL_EXHAUSTED
                                     : QuantityConsumptionStatus::SATISFIED;
        return result;
    }

    void PriceLevel::removeOrder(std::list<Order>::iterator itr) {
        Order targetOrder = *itr;
        quantity -= targetOrder.getRemainingQuantity();
        orders.erase(itr);
    }

    bool PriceLevel::isEmpty() const{
        return orders.empty();
    }

    unsigned int PriceLevel::getAggregateQuantity() const {
        return quantity;
    }
    unsigned int PriceLevel::getOrdersQuantity() const {
        return orders.size();
    }
}