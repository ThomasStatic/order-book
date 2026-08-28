#include "order_book/order_book.hpp"

namespace order_book {
    const Order& OrderBook::getBestBid() const{
        const PriceLevel& bestBidLevel = bids.begin()->second;
        return bestBidLevel.getOldestOrder();
    }

    const Order& OrderBook::getBestAsk() const{
        const PriceLevel& bestAskLevel = asks.begin()->second;
        return bestAskLevel.getOldestOrder();
    }

    bool OrderBook::hasBids() const {
        return !bids.empty();
    }

    bool OrderBook::hasAsks() const {
        return !asks.empty();
    }

    void OrderBook::addOrder(Order order) {
        PriceLevel* level = nullptr;
        if(order.getSide() == Side::BUY) {
            auto itr = bids.find(order.getPriceTick());

            if(itr != bids.end()) {
                level = &itr->second;

            }
            else {
                auto levelItr = bids.try_emplace(
                    order.getPriceTick(), order.getPriceTick(), order.getSide()).first;
                level = &levelItr->second;
            }
        }
        else {
            auto itr = asks.find(order.getPriceTick());

            if(itr != asks.end()) {
                level = &itr->second;;
            }
            else {
                auto levelItr = asks.try_emplace(
                    order.getPriceTick(), order.getPriceTick(), order.getSide()).first;
                level = &levelItr->second;
            }
        }

        OrderLocation location = level->addOrder(order);
        orderIndex.insert({order.getOrderId(), location});
    }

    OrderLocation OrderBook::findActiveOrder(OrderId orderId) const {
        auto orderItr = orderIndex.find(orderId);

        if(orderItr != orderIndex.end()) {
            return orderItr->second;
        }

        throw std::invalid_argument("order id not found in order index error");

    }

    void OrderBook::removeOrder(OrderId orderId) {
        auto orderItr = orderIndex.find(orderId);

        if(orderItr != orderIndex.end()) {
            OrderLocation location = orderItr->second;

            if(location.side == Side::BUY) {
                auto bidsItr = bids.find(location.tickPrice);

                if(bidsItr != bids.end()) {
                    bidsItr->second.removeOrder(location.itr);
                    if(bidsItr->second.isEmpty()) {
                        bids.erase(bidsItr->first);
                    }
                }
                else {
                    throw std::invalid_argument("price level not found in bids error");
                }
            }
            else if(location.side == Side::SELL) {
                auto asksItr = asks.find(location.tickPrice);

                if(asksItr != asks.end()) {
                    asksItr->second.removeOrder(location.itr);
                    if(asksItr->second.isEmpty()) {
                        asks.erase(asksItr->first);
                    }
                }
                else {
                    throw std::invalid_argument("price level not found in asks error");
                }
            }

            orderIndex.erase(orderItr);
            return;
        }

        throw std::invalid_argument("order id not found in order index error");
    }
}