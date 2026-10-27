#include <algorithm>
#include <stdexcept>

#include "order_book/price_level.hpp"

namespace order_book {
    PriceLevel::PriceLevel(Price price, Side s, NodePool* pool)
        : tickPrice(price), side(s), orders(PoolAllocator<Order>(pool)) {}

    PriceLevel::PriceLevel(Price price, Side s, Order initialOrder): PriceLevel(price, s) {
        addOrder(initialOrder);
    }

    OrderLocation PriceLevel::addOrder(Order newOrder) {
        OrderQueue::iterator itr = orders.insert(orders.end(), newOrder);
        quantity += newOrder.getRemainingQuantity();

        OrderLocation location(tickPrice, side, itr, this);
        return location;
    }

    const Order& PriceLevel::getOldestOrder() const{
        return orders.front();
    }

    OrderFill PriceLevel::fillOldestOrder(Quantity fillQuant) {
        if(fillQuant == 0) {
            throw std::invalid_argument("filling zero quantity from a price level error");
        }
        if(orders.empty()) {
            throw std::runtime_error("filling an empty price level error");
        }

        Order& oldestOrder = orders.front();

        const Quantity availableQuant = oldestOrder.getRemainingQuantity();
        const Quantity fillAmount = std::min(fillQuant, availableQuant);
        oldestOrder.fillQuantity(fillAmount);
        quantity -= fillAmount;

        OrderFill fill{oldestOrder.getOrderId(), fillAmount, false};
        if (oldestOrder.getRemainingQuantity() == 0) {
            fill.fullyFilled = true;
            orders.pop_front();
        }
        return fill;
    }

    void PriceLevel::removeOrder(OrderQueue::iterator itr) {
        // Read the remaining quantity first: erasing invalidates itr. It is the remaining
        // quantity, not the initial one, that the aggregate holds for this order.
        const Quantity remaining = itr->getRemainingQuantity();
        quantity -= remaining;
        orders.erase(itr);
    }

    bool PriceLevel::isEmpty() const{
        return orders.empty();
    }

    std::uint64_t PriceLevel::getAggregateQuantity() const {
        return quantity;
    }
    unsigned int PriceLevel::getOrderCount() const {
        // Live orders have distinct nonzero OrderIds, so a level never holds more than an
        // unsigned int can count.
        return static_cast<unsigned int>(orders.size());
    }
}