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
                level = new PriceLevel(order.getPriceTick(), order.getSide());
                bids.emplace(order.getPriceTick(), level);
            }
        }
        else {
            auto itr = asks.find(order.getPriceTick());

            if(itr != asks.end()) {
                level = &itr->second;;
            }
            else {
                level = new PriceLevel(order.getPriceTick(), order.getSide());
                asks.emplace(order.getPriceTick(), level);
            }
        }

        OrderLocation location = level->addOrder(order);
        orderIndex.insert({order.getOrderId(), location});
    }
}