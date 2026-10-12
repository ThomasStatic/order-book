#include <algorithm>
#include <optional>

#include "order_book/order_book.hpp"

namespace order_book {
    Order OrderBook::getBestBid() const {
        if(bids.empty()) {
            throw std::runtime_error("best bid requested from an empty bid side error");
        }
        return bids.begin()->second.getOldestOrder();
    }

    Order OrderBook::getBestAsk() const {
        if(asks.empty()) {
            throw std::runtime_error("best ask requested from an empty ask side error");
        }
        return asks.begin()->second.getOldestOrder();
    }

    bool OrderBook::hasBids() const {
        return !bids.empty();
    }

    bool OrderBook::hasAsks() const {
        return !asks.empty();
    }

    void OrderBook::validateNewOrder(const Order& order) const {
        if(order.getRemainingQuantity() == 0 || !order.orderIsActive()) {
            throw std::invalid_argument("adding an inactive order error");
        }
        if(orderIndex.find(order.getOrderId()) != orderIndex.end()) {
            throw std::invalid_argument("duplicate active order id error");
        }
    }

    void OrderBook::addOrder(Order order) {
        validateNewOrder(order);

        auto rest = [this, &order](auto& levels) {
            const auto levelItr = levels.try_emplace(
                order.getPriceTick(), order.getPriceTick(), order.getSide()).first;
            PriceLevel& level = levelItr->second;

            std::optional<OrderLocation> location;
            try {
                location = level.addOrder(order);
                orderIndex.emplace(order.getOrderId(), *location);
            }
            catch(...) {
                // Unwind so a failed allocation leaves no unindexed order and no empty level.
                if(location) {
                    level.removeOrder(location->itr);
                }
                if(level.isEmpty()) {
                    levels.erase(levelItr);
                }
                throw;
            }
        };

        if(order.getSide() == Side::BUY) {
            rest(bids);
        }
        else {
            rest(asks);
        }
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
            // Make room first: once executeOnce has changed the book, recording its trade
            // must not be able to throw. Doubling keeps push_back amortized O(1).
            if(trades.size() == trades.capacity()) {
                trades.reserve(trades.empty() ? 4 : trades.capacity() * 2);
            }
            trades.push_back(executeOnce(incoming));
        }
        return trades;
    }

    std::vector<Trade> OrderBook::submitOrder(Order incoming) {
        // Reject before matching: failing only when the remainder rests would leave
        // trades executed for a submission that then throws.
        validateNewOrder(incoming);

        // Not atomic: if an allocation fails mid-match or while resting the remainder,
        // executions already made stay made and their trades are lost with the exception.
        // The book itself is left consistent.

        std::vector<Trade> trades = matchOrder(incoming);
        if(incoming.getRemainingQuantity() > 0) {
            addOrder(incoming);
        }
        return trades;
    }

    Order OrderBook::findActiveOrder(OrderId orderId) const {
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
                        bids.erase(bidsItr);
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
                        asks.erase(asksItr);
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
        auto consume = [this, side, price, quantity](auto& levels) {
            const auto levelItr = levels.find(price);
            if(levelItr == levels.end()) {
                throw std::invalid_argument(side == Side::BUY ? "price level not found in bids error"
                                                              : "price level not found in asks error");
            }
            PriceLevel& level = levelItr->second;

            ConsumptionResult result{QuantityConsumptionStatus::INVALID_REQUEST, quantity, {}};
            if(quantity == 0) {
                return result;
            }
            // At most one fill per resting order, so recording a fill can never throw
            // after the level has changed.
            result.fills.reserve(level.getOrdersQuantity());

            // Unindex each fully filled order as soon as it leaves the level, so a throw
            // partway through cannot strand an index entry pointing at a popped order.
            while(result.unfilledQuantity > 0 && !level.isEmpty()) {
                const OrderFill fill = level.fillOldestOrder(result.unfilledQuantity);
                if(fill.fullyFilled) {
                    orderIndex.erase(fill.orderId);
                }
                result.unfilledQuantity -= fill.filledQuantity;
                result.fills.push_back(fill);
            }

            if(level.isEmpty()) {
                result.status = QuantityConsumptionStatus::SATISFIED;
                levels.erase(levelItr);
            }
            else {
                result.status = QuantityConsumptionStatus::LEVEL_EXHAUSTED;
            }
            return result;
        };

        return side == Side::BUY ? consume(bids) : consume(asks);
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