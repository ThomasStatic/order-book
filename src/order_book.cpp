#include <algorithm>

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

    bool OrderBook::crossesBestOpposing(const Order& incoming) const {
        if(incoming.getSide() == Side::BUY) {
            return hasAsks() && incoming.getPriceTick() >= asks.begin()->first;
        }
        return hasBids() && incoming.getPriceTick() <= bids.begin()->first;
    }

    Trade OrderBook::executeOnce(Order& incoming) {
        // Every check runs and the trade is built before anything is mutated, so a throw
        // leaves the book, its index, and the incoming order untouched.
        if(incoming.getRemainingQuantity() == 0 || !incoming.orderIsActive()) {
            throw std::runtime_error("executing an inactive incoming order error");
        }
        if(!crossesBestOpposing(incoming)) {
            throw std::runtime_error("executing an incoming order that does not cross error");
        }

        const bool restingIsAsk = incoming.getSide() == Side::BUY;
        const Price restingPrice = restingIsAsk ? asks.begin()->first : bids.begin()->first;
        PriceLevel& level = restingIsAsk ? asks.begin()->second : bids.begin()->second;

        // Copy what the trade needs before filling: a fully filled resting order is
        // popped from the level, invalidating both this reference and its indexed iterator.
        const Order& resting = level.getOldestOrder();
        if(!resting.orderIsActive()) {
            throw std::runtime_error("executing against an inactive resting order error");
        }
        const OrderId restingId = resting.getOrderId();
        const unsigned int execQuantity = std::min(incoming.getRemainingQuantity(),
                                                   resting.getRemainingQuantity());
        const Trade trade(nextTradeId, restingId, incoming.getOrderId(),
                          restingPrice, execQuantity, incoming.getSide());

        // Nothing below can throw: both orders are active and execQuantity is positive
        // and no larger than either remaining quantity.
        const OrderFill fill = level.fillOldestOrder(execQuantity);
        incoming.fillQuantity(execQuantity);
        ++nextTradeId;

        if(fill.fullyFilled) {
            orderIndex.erase(restingId);
        }
        if(level.isEmpty()) {
            if(restingIsAsk) {
                asks.erase(asks.begin());
            }
            else {
                bids.erase(bids.begin());
            }
        }

        return trade;
    }

    std::vector<Trade> OrderBook::matchOrder(Order& incoming) {
        std::vector<Trade> trades;
        while(incoming.getRemainingQuantity() > 0 && crossesBestOpposing(incoming)) {
            trades.push_back(executeOnce(incoming));
        }
        return trades;
    }

    std::vector<Trade> OrderBook::submitOrder(Order incoming) {
        std::vector<Trade> trades = matchOrder(incoming);
        if(incoming.getRemainingQuantity() > 0) {
            addOrder(incoming);
        }
        return trades;
    }

    const Order& OrderBook::findActiveOrder(OrderId orderId) const {
        auto orderItr = orderIndex.find(orderId);

        if(orderItr != orderIndex.end()) {
            return *orderItr->second.itr;
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

    ConsumptionResult OrderBook::consumeLevel(Side side, Price price, unsigned int quantity) {
        ConsumptionResult result;
        if(side == Side::BUY) {
            auto bidsItr = bids.find(price);
            if(bidsItr == bids.end()) {
                throw std::invalid_argument("price level not found in bids error");
            }

            result = bidsItr->second.consumeQuantity(quantity);
            if(bidsItr->second.isEmpty()) {
                bids.erase(bidsItr);
            }
        }
        else {
            auto asksItr = asks.find(price);
            if(asksItr == asks.end()) {
                throw std::invalid_argument("price level not found in asks error");
            }

            result = asksItr->second.consumeQuantity(quantity);
            if(asksItr->second.isEmpty()) {
                asks.erase(asksItr);
            }
        }

        for(const OrderFill& fill : result.fills) {
            if(fill.fullyFilled) {
                orderIndex.erase(fill.orderId);
            }
        }
        return result;
    }

    BookSnapshot OrderBook::snapshot(unsigned int depth) const {
        unsigned int totalActiveOrders = 0;
        
        unsigned int bidsCount = 0;
        std::vector<PriceLevelSummary> bestBids;
        for(auto itr = bids.begin(); itr != bids.end() && bidsCount < depth; ++itr, ++bidsCount) {
            PriceLevelSummary topBid;
            topBid.price = itr->first;
            unsigned int ordersQuantity = itr->second.getOrdersQuantity();
            topBid.orderQuantity = ordersQuantity;
            totalActiveOrders += ordersQuantity;
            topBid.totalQuantity = itr->second.getAggregateQuantity();
            bestBids.push_back(topBid);
        }

        unsigned int asksCount = 0;
        std::vector<PriceLevelSummary> bestAsks;
        for(auto itr = asks.begin(); itr != asks.end() && asksCount < depth; ++itr, ++asksCount) {
            PriceLevelSummary topAsk;
            topAsk.price = itr->first;
            unsigned int ordersQuantity = itr->second.getOrdersQuantity();
            topAsk.orderQuantity = ordersQuantity;
            totalActiveOrders += ordersQuantity;
            topAsk.totalQuantity = itr->second.getAggregateQuantity();
            bestAsks.push_back(topAsk);
        }

        BookSnapshot snapshot;
        snapshot.asks = bestAsks;
        snapshot.bids = bestBids;
        snapshot.totalActiveVisibleOrders = totalActiveOrders;
        return snapshot;
    }
}