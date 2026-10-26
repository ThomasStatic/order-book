#include <algorithm>
#include <optional>
#include <utility>

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

    std::optional<RejectionReason> OrderBook::validateNewOrder(const Order& order) const {
        if(const std::optional<RejectionReason> reason = order.malformedReason()) {
            return reason;
        }
        if(!order.orderIsActive()) {
            return RejectionReason::ORDER_NOT_ACTIVE;
        }
        if(orderIndex.find(order.getOrderId()) != orderIndex.end()) {
            return RejectionReason::DUPLICATE_ORDER_ID;
        }
        return std::nullopt;
    }

    SubmissionResults OrderBook::addOrder(Order order) {
        const InvariantBoundary boundary(*this, "addOrder");
        if(const std::optional<RejectionReason> reason = validateNewOrder(order)) {
            throw std::invalid_argument(rejectionMessage(*reason));
        }
        // Adding never matches, so resting a crossing order would leave the book crossed.
        if(crossesBestOpposing(order)) {
            throw std::invalid_argument(rejectionMessage(RejectionReason::WOULD_CROSS_BOOK));
        }
        order.assignSequenceNum(nextSequenceNum);
        // Built before the order rests, so a failed allocation leaves the book untouched.
        SubmissionResults result = SubmissionResults::accepted(
            {order.getOrderId(), order.getSide(), order.getPriceTick(), order.getRemainingQuantity()}, {},
            order.getRemainingQuantity());
        restOrder(order);
        // Consumed only once the order rests, so a rejected or rolled-back add leaves no gap.
        advanceSequenceNum();
        return result;
    }

    void OrderBook::advanceSequenceNum() {
        ++nextSequenceNum;
        if(nextSequenceNum == 0) {
            nextSequenceNum = 1;
        }
    }

    void OrderBook::advanceTradeId() {
        ++nextTradeId;
        if(nextTradeId == 0) {
            nextTradeId = 1;
        }
    }

    void OrderBook::restOrder(const Order& order) {
        auto rest = [this, &order](auto& levels) {
            const auto levelItr = levels.try_emplace(
                order.getPriceTick(), order.getPriceTick(), order.getSide()).first;
            PriceLevel& level = levelItr->second;

            std::optional<OrderLocation> location;
            try {
                location = level.addOrder(order);
                if(!orderIndex.emplace(order.getOrderId(), *location).second) {
                    // emplace keeps the existing entry, which would leave this order resting
                    // unindexed; throwing here unwinds it like a failed allocation.
                    throw std::runtime_error("resting an order whose id is already indexed error");
                }
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

        // Every level in the book should hold an order, but reading the front of an empty
        // one is undefined, so check rather than assume.
        if(level.isEmpty()) {
            throw std::runtime_error("executing against an empty price level error");
        }

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
        // Unindex a popped order at once, so its entry never outlives its list element.
        if(fill.fullyFilled) {
            orderIndex.erase(restingId);
        }
        incoming.fillQuantity(execQuantity);
        advanceTradeId();

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

    SubmissionTrades OrderBook::matchOrder(Order& incoming) {
        SubmissionTrades trades;
        while(incoming.getRemainingQuantity() > 0 && crossesBestOpposing(incoming)) {
            // Make room first: once executeOnce has changed the book, recording its trade
            // must not be able to throw. The first few fit inline; beyond them, doubling
            // keeps push_back amortized O(1).
            if(trades.size() == trades.capacity()) {
                trades.reserve(trades.capacity() * 2);
            }
            trades.push_back(executeOnce(incoming));
        }
        return trades;
    }

    SubmissionResults OrderBook::submitOrder(Order incoming) {
        const InvariantBoundary boundary(*this, "submitOrder");
        // Reject before matching: failing only when the remainder rests would leave
        // trades executed for a submission that is then refused.
        if(const std::optional<RejectionReason> reason = validateNewOrder(incoming)) {
            return SubmissionResults::rejected(incoming.getOrderId(), incoming.getRemainingQuantity(), *reason);
        }

        // Accepted: numbered once, before matching, and the remainder rests with this same
        // number. Consumed even if matching throws, since executions may already stand.
        incoming.assignSequenceNum(nextSequenceNum);
        advanceSequenceNum();
        // Captured before matching, so it reports the quantity as submitted.
        const OrderAccepted acceptance{incoming.getOrderId(), incoming.getSide(), incoming.getPriceTick(),
                                       incoming.getRemainingQuantity()};

        // Not atomic: if an allocation fails mid-match, while building the result, or while
        // resting the remainder,
        // executions already made stay made and their trades and events are lost with the
        // exception.
        // The book itself is left consistent.

        // The result, events included, is built before the remainder rests, so resting is
        // the last step that can fail: a remainder is never left resting behind a throw.
        SubmissionTrades trades = matchOrder(incoming);
        SubmissionResults result = SubmissionResults::accepted(acceptance, std::move(trades),
                                                               incoming.getRemainingQuantity());
        if(incoming.getRemainingQuantity() > 0) {
            // Rests at the incoming limit price (fills never change it), not at the price
            // of its last execution. Validation already passed, so it cannot be rejected now.
            restOrder(incoming);
        }
        return result;
    }

    std::size_t OrderBook::activeOrderCount() const {
        return orderIndex.size();
    }

    void OrderBook::reserve(std::size_t orders) {
        // No InvariantBoundary: the book's contents do not change. A rehash that throws
        // (other than from the hash, which cannot) leaves the index as it was.
        orderIndex.reserve(orders);
    }

    Order OrderBook::findActiveOrder(OrderId orderId) const {
        auto orderItr = orderIndex.find(orderId);

        if(orderItr != orderIndex.end()) {
            return *orderItr->second.itr;
        }

        throw std::invalid_argument("order id not found in order index error");

    }

    CancellationResult OrderBook::cancelOrder(OrderId orderId) {
        const InvariantBoundary boundary(*this, "cancelOrder");
        const auto indexItr = orderIndex.find(orderId);
        if(indexItr == orderIndex.end()) {
            return CancellationResult::rejected(orderId, RejectionReason::ORDER_NOT_FOUND);
        }
        const OrderLocation location = indexItr->second;
        // Built before anything is mutated, so nothing after the removal can throw. (Its one
        // event is held inline, so building it does not allocate either.)
        CancellationResult result = CancellationResult::cancelled(orderId, location.itr->getRemainingQuantity());

        auto cancel = [this, indexItr, &location](auto& levels) {
            // The entry's own level handle, not a search of the map. An indexed order always
            // has its level; if the handle does not name a level at the entry's price and side,
            // fail before mutating anything.
            PriceLevel* const level = location.level;
            if(level == nullptr || level->tickPrice != location.tickPrice || level->side != location.side) {
                throw std::runtime_error(location.side == Side::BUY ? "indexed order's level handle does not match its bid entry error"
                                                                    : "indexed order's level handle does not match its ask entry error");
            }

            // Nothing below can throw. Unindex straight after erasing, so the entry never
            // holds the erased element's dangling iterator.
            level->removeOrder(location.itr);
            orderIndex.erase(indexItr);
            if(level->isEmpty()) {
                levels.erase(location.tickPrice);
            }
        };

        if(location.side == Side::BUY) {
            cancel(bids);
        }
        else {
            cancel(asks);
        }
        return result;
    }

    ConsumptionResult OrderBook::consumeLevel(Side side, Price price, unsigned int quantity) {
        const InvariantBoundary boundary(*this, "consumeLevel");
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
        snapshot.asks = std::move(bestAsks);
        snapshot.bids = std::move(bestBids);
        snapshot.totalActiveVisibleOrders = totalActiveOrders;
        return snapshot;
    }
}