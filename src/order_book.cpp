#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#include "order_book/order_book.hpp"
#include "order_checks.hpp"

namespace order_book {
    std::optional<RejectionReason> malformedReason(const OrderRequest& request) {
        return checks::requestReason(request);
    }

    std::optional<RejectionReason> malformedReason(const ReplaceRequest& request) {
        return checks::replaceReason(request);
    }

    OrderBook::OrderBook(OrderBook&& other) noexcept
        : nodePool(std::move(other.nodePool)),
          orderIndex(std::move(other.orderIndex)),
          pendingIndex(std::move(other.pendingIndex)),
          bids(std::move(other.bids)),
          asks(std::move(other.asks)),
          buyStops(std::move(other.buyStops)),
          sellStops(std::move(other.sellStops)),
          triggeredStops(std::move(other.triggeredStops)),
          lastTradePrice(other.lastTradePrice),
          reservedCapacity(std::exchange(other.reservedCapacity, BookCapacity{})),
          reservedChunks(std::exchange(other.reservedChunks, 0)),
          nextTradeId(other.nextTradeId),
          nextSequenceNum(other.nextSequenceNum) {}

    OrderBook& OrderBook::operator=(OrderBook&& other) noexcept {
        if(this != &other) {
            // Free this book's nodes while their pool still exists, then take the other's.
            orderIndex.clear();
            pendingIndex.clear();
            bids.clear();
            asks.clear();
            buyStops.clear();
            sellStops.clear();
            triggeredStops.reset();
            orderIndex = std::move(other.orderIndex);
            pendingIndex = std::move(other.pendingIndex);
            bids = std::move(other.bids);
            asks = std::move(other.asks);
            buyStops = std::move(other.buyStops);
            sellStops = std::move(other.sellStops);
            triggeredStops = std::move(other.triggeredStops);
            lastTradePrice = other.lastTradePrice;
            nextTradeId = other.nextTradeId;
            nextSequenceNum = other.nextSequenceNum;
            reservedCapacity = std::exchange(other.reservedCapacity, BookCapacity{});
            reservedChunks = std::exchange(other.reservedChunks, 0);
            nodePool = std::move(other.nodePool);
        }
        return *this;
    }

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
        if(!order.isActive()) {
            return RejectionReason::ORDER_NOT_ACTIVE;
        }
        if(isLive(order.getOrderId())) {
            return RejectionReason::DUPLICATE_ORDER_ID;
        }
        return std::nullopt;
    }

    bool OrderBook::isLive(OrderId orderId) const {
        return orderIndex.find(orderId) != orderIndex.end() || pendingIndex.find(orderId) != pendingIndex.end();
    }

    std::optional<RejectionReason> OrderBook::validateNewRequest(const OrderRequest& request) const {
        if(const std::optional<RejectionReason> reason = checks::requestReason(request)) {
            return reason;
        }
        if(isLive(request.id)) {
            return RejectionReason::DUPLICATE_ORDER_ID;
        }
        return std::nullopt;
    }

    SubmissionResult OrderBook::addOrder(Order order) {
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
        SubmissionResult result = SubmissionResult::accepted(
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

    PriceLevel& OrderBook::checkedLevel(const OrderLocation& location) {
        PriceLevel* const level = location.level;
        if(level == nullptr || level->tickPrice != location.tickPrice || level->side != location.side) {
            throw std::runtime_error(location.side == Side::BUY ? "indexed order's level handle does not match its bid entry error"
                                                                : "indexed order's level handle does not match its ask entry error");
        }
        return *level;
    }

    void OrderBook::restOrder(const Order& order, OwnerId owner) {
        auto rest = [this, &order, owner](auto& levels) {
            const auto levelItr = levels.try_emplace(
                order.getPriceTick(), order.getPriceTick(), order.getSide(), nodePool.get()).first;
            PriceLevel& level = levelItr->second;

            std::optional<OrderLocation> location;
            try {
                location = level.addOrder(order, owner);
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

    std::optional<OrderBook::FillPlan> OrderBook::fillOrKillPlan(const Order& incoming, const Instructions& how) const {
        const Quantity wanted = incoming.getRemainingQuantity();
        const Price limit = incoming.getPriceTick();
        const bool buying = incoming.getSide() == Side::BUY;
        // With an owner, every crossed order is visited and its owner probed.
        auto walkOrders = [&](const auto& levels) -> std::optional<FillPlan> {
            std::uint64_t available = 0;
            FillPlan plan{0, 0};
            for(const auto& [price, level] : levels) {
                if(buying ? price > limit : price < limit) {
                    break;
                }
                for(const RestingOrder& order : level.orders) {
                    if(order.owner == how.owner) {
                        if(how.selfTradePrevention != SelfTradePrevention::CANCEL_RESTING) {
                            return std::nullopt;  // matching would stop here, short of filling
                        }
                        ++plan.preventions;
                        continue;
                    }
                    ++plan.trades;
                    available += order.getRemainingQuantity();
                    if(available >= wanted) {
                        return plan;
                    }
                }
            }
            return std::nullopt;
        };
        auto walk = [&](const auto& levels) -> std::optional<FillPlan> {
            if(how.owner != 0) {
                return walkOrders(levels);
            }
            std::uint64_t available = 0;
            std::size_t trades = 0;
            for(const auto& [price, level] : levels) {
                if(buying ? price > limit : price < limit) {
                    break;
                }
                if(available + level.getAggregateQuantity() < wanted) {
                    available += level.getAggregateQuantity();
                    trades += level.getOrderCount();
                    continue;
                }
                // The level that completes it: count the orders it takes, oldest first.
                for(const Order& order : level.orders) {
                    ++trades;
                    available += order.getRemainingQuantity();
                    if(available >= wanted) {
                        return FillPlan{trades, 0};
                    }
                }
                break;
            }
            return std::nullopt;
        };
        return buying ? walk(asks) : walk(bids);
    }

    Trade OrderBook::executeOnce(Order& incoming) {
        // Every check runs and the trade is built before anything is mutated, so a throw
        // leaves the book, its index, and the incoming order untouched.
        if(incoming.getRemainingQuantity() == 0 || !incoming.isActive()) {
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
        if(!resting.isActive()) {
            throw std::runtime_error("executing against an inactive resting order error");
        }
        const OrderId restingId = resting.getOrderId();
        const Quantity execQuantity = std::min(incoming.getRemainingQuantity(),
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
        lastTradePrice = restingPrice;
        triggerStops(restingPrice);

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

    bool OrderBook::matchOrder(Order& incoming, const Instructions& how, ResultRecorder& recorder) {
        while(incoming.getRemainingQuantity() > 0 && crossesBestOpposing(incoming)) {
            // Self-trade prevention, probed only for an incoming order with an owner, against
            // the order it would trade with next.
            if(how.owner != 0) {
                const RestingOrder& resting = incoming.getSide() == Side::BUY ? asks.begin()->second.orders.front()
                                                                              : bids.begin()->second.orders.front();
                if(resting.owner == how.owner) {
                    const auto entry = orderIndex.find(resting.getOrderId());
                    if(entry == orderIndex.end()) {
                        throw std::runtime_error("resting order missing from the order index error");
                    }
                    const SelfTradePrevention mode = how.selfTradePrevention;
                    const bool removesResting = mode != SelfTradePrevention::CANCEL_INCOMING;
                    // Recorded (with room made first) before the removal, which cannot throw once
                    // its guard has passed, so the event is never lost behind a change.
                    recorder.makeRoom(0, 1);
                    recorder.prevented(SelfTradePrevented{incoming.getOrderId(), resting.getOrderId(),
                                                          entry->second.tickPrice,
                                                          removesResting ? resting.getRemainingQuantity() : 0},
                                       incoming.getSide());
                    if(removesResting) {
                        removeResting(entry);
                    }
                    if(mode != SelfTradePrevention::CANCEL_RESTING) {
                        return true;
                    }
                    continue;
                }
            }
            // Make room first: once executeOnce has changed the book, recording its trade
            // must not be able to throw. The first few fit inline; beyond them, doubling
            // keeps appending amortized O(1).
            recorder.makeRoom(1, 1);
            recorder.trade(executeOnce(incoming));
        }
        return false;
    }

    SubmissionResult OrderBook::submitOrder(Order incoming) {
        const InvariantBoundary boundary(*this, "submitOrder");
        // Reject before matching: failing only when the remainder rests would leave
        // trades executed for a submission that is then refused.
        if(const std::optional<RejectionReason> reason = validateNewOrder(incoming)) {
            return SubmissionResult::rejected(incoming.getOrderId(), incoming.getRemainingQuantity(), *reason);
        }
        ResultRecorder recorder(OrderAccepted{incoming.getOrderId(), incoming.getSide(), incoming.getPriceTick(),
                                              incoming.getRemainingQuantity()});
        const TriggeredStopsGuard discardOnThrow(*this);
        runIncoming(incoming, Instructions{}, recorder);
        runCascade(recorder);
        return recorder.finish();
    }

    // The steps both overloads of a command share. Inlined by force: the by-value overloads
    // must cost what they did before the out overloads existed, and a paired run of the
    // replace-heavy workloads against v2.0.0 shows a call here.
#if defined(_MSC_VER)
#define ORDER_BOOK_SHARED_STEP __forceinline
#else
#define ORDER_BOOK_SHARED_STEP inline __attribute__((always_inline))
#endif

    ORDER_BOOK_SHARED_STEP void OrderBook::runRequest(const OrderRequest& request, ResultRecorder& recorder) {
        if(isStopType(request.type)) {
            submitStop(request, recorder);  // makes no trades, so triggers nothing
            return;
        }
        Order incoming = incomingOrder(request);
        if(request.postOnly) {
            runPostOnly(incoming, request.owner, recorder);
        }
        else {
            runIncoming(incoming, Instructions{request.timeInForce, request.owner, request.selfTradePrevention}, recorder);
        }
        runCascade(recorder);
    }

    ORDER_BOOK_SHARED_STEP std::optional<RejectionReason> OrderBook::planReplace(const ReplaceRequest& request, ReplacePlan& plan) {
        // The malformed checks first, then the state checks; nothing has changed yet.
        std::optional<RejectionReason> reason = checks::replaceReason(request);
        if(!reason && pendingIndex.find(request.id) != pendingIndex.end()) {
            reason = RejectionReason::ORDER_NOT_REPLACEABLE;
        }
        const auto entry = reason ? orderIndex.end() : orderIndex.find(request.id);
        if(!reason && entry == orderIndex.end()) {
            reason = RejectionReason::ORDER_NOT_FOUND;
        }
        if(reason) {
            return reason;
        }
        const Order& resting = *entry->second.itr;
        const Quantity executed = resting.getInitialQuantity() - resting.getRemainingQuantity();
        if(request.quantity <= executed) {
            return RejectionReason::REPLACE_QUANTITY_TOO_LOW;
        }

        const Quantity open = request.quantity - executed;
        const bool keepsPriority = request.price == resting.getPriceTick() && request.quantity <= resting.getInitialQuantity();
        plan = ReplacePlan{entry, executed, OrderReplaced{request.id, resting.getSide(), request.price, request.quantity,
                                                          open, keepsPriority}};
        return std::nullopt;
    }

    ORDER_BOOK_SHARED_STEP void OrderBook::runReplace(const ReplaceRequest& request, const ReplacePlan& plan, ResultRecorder& recorder) {
        const OrderIndex::iterator entry = plan.entry;
        const Quantity open = plan.replacement.openQuantity;
        Order& resting = *entry->second.itr;

        if(plan.replacement.priorityKept) {
            // In place, as cancelOrder reaches the level: through the entry's own handle, checked
            // before anything changes. Nothing below allocates or throws.
            PriceLevel* const level = &checkedLevel(entry->second);
            level->quantity -= resting.getRemainingQuantity() - open;
            resting.initialQuantity = request.quantity;
            resting.remainingQuantity = open;
            entry->second.itr->owner = request.owner;
            return;
        }

        // Losing priority, it re-enters as if it had just arrived, at its new price.
        Order incoming = resting;
        incoming.priceTick = request.price;
        incoming.initialQuantity = request.quantity;
        incoming.remainingQuantity = open;
        const bool crosses = crossesBestOpposing(incoming);
        if(request.postOnly && crosses) {
            // Two events, inline, and a removal: nothing allocates or throws after the guard.
            removeResting(entry);
            recorder.expire(open);
            advanceSequenceNum();
            return;
        }
        if(!crosses) {
            moveToBack(entry, request.price, request.quantity, open, request.owner);
            advanceSequenceNum();
            return;
        }
        // Crossing: it leaves its level, then matches like a new GTC limit order, with
        // submitOrder's contract, and runs the cascade its trades trigger.
        removeResting(entry);
        runIncoming(incoming, Instructions{TimeInForce::GTC, request.owner, request.selfTradePrevention}, recorder);
        runCascade(recorder);
    }

    SubmissionResult OrderBook::submitOrder(OrderRequest request) {
        const InvariantBoundary boundary(*this, "submitOrder");
        if(const std::optional<RejectionReason> reason = validateNewRequest(request)) {
            return SubmissionResult::rejected(request.id, request.quantity, *reason);
        }
        // Its price is 0 for an order with none (a market or STOP order).
        ResultRecorder recorder(OrderAccepted{request.id, request.side, request.price, request.quantity});
        const TriggeredStopsGuard discardOnThrow(*this);
        runRequest(request, recorder);
        return recorder.finish();
    }

    void OrderBook::submitOrder(OrderRequest request, SubmissionResult& out) {
        const InvariantBoundary boundary(*this, "submitOrder");
        if(const std::optional<RejectionReason> reason = validateNewRequest(request)) {
            ResultRecorder::reject(out, request.id, request.quantity, *reason);
            return;
        }
        ResultRecorder recorder(OrderAccepted{request.id, request.side, request.price, request.quantity}, out);
        const TriggeredStopsGuard discardOnThrow(*this);
        runRequest(request, recorder);
        recorder.finish(out);
    }

    Order OrderBook::incomingOrder(const OrderRequest& request) const {
        // An order with no price (a market order, or a triggered STOP) has no limit, so it runs
        // as a limit at the furthest price on its side, which every resting price is within; it
        // never rests, so that price is never seen.
        const Price limit = !hasNoPrice(request.type) ? request.price
                          : request.side == Side::BUY ? std::numeric_limits<Price>::max()
                                                      : Price{1};
        // Well formed, so the constructor cannot refuse it; it is numbered when it runs.
        return Order(request.id, limit, request.quantity, nextSequenceNum, request.side);
    }

    void OrderBook::runIncoming(Order& incoming, const Instructions& how, ResultRecorder& recorder) {
        if(how.timeInForce == TimeInForce::FOK) {
            runFillOrKill(incoming, how, recorder);
            return;
        }
        // Accepted: numbered once, before matching, and the remainder rests with this same
        // number. Consumed even if matching throws, since executions may already stand.
        incoming.assignSequenceNum(nextSequenceNum);
        advanceSequenceNum();

        // Not atomic: if an allocation fails mid-match, or while recording the expiry or
        // resting the remainder, executions already made stay made and their trades and events
        // are lost with the exception. The book itself is left consistent.
        const bool stopped = matchOrder(incoming, how, recorder);
        if(incoming.getRemainingQuantity() == 0) {
            return;
        }
        // Stopped by self-trade prevention, even a GTC remainder expires: it is at or through
        // its own resting order's price, so resting it would cross the book.
        if(how.timeInForce == TimeInForce::IOC || stopped) {
            // Changes nothing in the book, so it may allocate here.
            recorder.expire(incoming.getRemainingQuantity());
            return;
        }
        // Everything this order reports is recorded, so resting it is its last step that can
        // fail: its remainder never rests behind a throw of its own. Rests at its limit price
        // (fills never change it), not at the price of its last execution.
        restOrder(incoming, how.owner);
    }

    void OrderBook::runFillOrKill(Order& incoming, const Instructions& how, ResultRecorder& recorder) {
        const std::optional<FillPlan> plan = fillOrKillPlan(incoming, how);
        incoming.assignSequenceNum(nextSequenceNum);
        if(!plan) {
            // Accepted then wholly expired. A submitted FOK's acceptance and expiry fit inline,
            // so nothing here allocates; a triggered stop's expiry may, before it is numbered.
            recorder.expire(incoming.getRemainingQuantity());
            advanceSequenceNum();
            return;
        }
        // Room for all it will record before anything is numbered or traded: the only step that
        // can fail. After it, matching cannot throw (executeOnce and the erasures it makes never
        // allocate), so the FOK either completes or changes nothing.
        recorder.reserve(plan->trades, plan->trades + plan->preventions);
        advanceSequenceNum();
        matchOrder(incoming, how, recorder);
    }

    void OrderBook::runPostOnly(Order& incoming, OwnerId owner, ResultRecorder& recorder) {
        if(crossesBestOpposing(incoming)) {
            // Its acceptance and expiry fit inline, so nothing here allocates or throws.
            recorder.expire(incoming.getRemainingQuantity());
            advanceSequenceNum();
            return;
        }
        // Rests exactly as addOrder does: numbered with the next number, which is consumed only
        // once it has rested, so a failed allocation leaves no trace.
        incoming.assignSequenceNum(nextSequenceNum);
        restOrder(incoming, owner);
        advanceSequenceNum();
    }

    ReplaceResult OrderBook::replaceOrder(const ReplaceRequest& request) {
        const InvariantBoundary boundary(*this, "replaceOrder");
        ReplacePlan plan;
        if(const std::optional<RejectionReason> reason = planReplace(request, plan)) {
            return ReplaceResult::rejected(request.id, *reason);
        }
        // Its OrderReplaced fits inline, so opening the result cannot throw.
        ResultRecorder recorder(plan.replacement, plan.executed);
        const TriggeredStopsGuard discardOnThrow(*this);
        runReplace(request, plan, recorder);
        return recorder.finishReplace();
    }

    void OrderBook::replaceOrder(const ReplaceRequest& request, ReplaceResult& out) {
        const InvariantBoundary boundary(*this, "replaceOrder");
        ReplacePlan plan;
        if(const std::optional<RejectionReason> reason = planReplace(request, plan)) {
            ResultRecorder::reject(out, request.id, *reason);
            return;
        }
        ResultRecorder recorder(plan.replacement, plan.executed, out);
        const TriggeredStopsGuard discardOnThrow(*this);
        runReplace(request, plan, recorder);
        recorder.finishReplace(out);
    }

    void OrderBook::moveToBack(OrderIndex::iterator entry, Price newPrice, Quantity total, Quantity open, OwnerId owner) {
        OrderLocation& location = entry->second;
        PriceLevel* const source = &checkedLevel(location);
        auto move = [&](auto& levels) {
            // The one step that can fail: a new level at the new price.
            PriceLevel& target = levels.try_emplace(newPrice, newPrice, location.side, nodePool.get()).first->second;
            // Nothing below allocates or throws. Splicing keeps the node, so the entry's
            // iterator stays valid; the allocators are equal, both from this book's pool.
            const OrderQueue::iterator node = location.itr;
            source->quantity -= node->getRemainingQuantity();
            target.orders.splice(target.orders.end(), source->orders, node);
            node->priceTick = newPrice;
            node->initialQuantity = total;
            node->remainingQuantity = open;
            node->sequenceNum = nextSequenceNum;
            target.quantity += open;
            const Price oldPrice = location.tickPrice;
            location.tickPrice = newPrice;
            location.level = &target;
            node->owner = owner;
            if(source->isEmpty()) {
                levels.erase(oldPrice);
            }
        };
        if(location.side == Side::BUY) {
            move(bids);
        }
        else {
            move(asks);
        }
    }

    void OrderBook::triggerStops(Price tradePrice) noexcept {
        if(!triggeredStops) {
            return;  // no stop has ever pended here
        }
        // Only stops whose trigger this trade meets: none is past the previous last trade price,
        // so these are exactly the band between it and this one, on one side, nearest first.
        auto trigger = [this, tradePrice](auto& levels, auto reached) {
            while(!levels.empty() && reached(levels.begin()->first)) {
                StopLevel& level = levels.begin()->second;
                for(PendingStop& stop : level.stops) {
                    pendingIndex.erase(stop.request.id);
                    stop.triggeredAt = tradePrice;
                }
                // The same nodes, moved: equal allocators, so this allocates nothing.
                triggeredStops->splice(triggeredStops->end(), level.stops);
                levels.erase(levels.begin());
            }
        };
        trigger(buyStops, [tradePrice](Price stopPrice) { return stopPrice <= tradePrice; });
        trigger(sellStops, [tradePrice](Price stopPrice) { return stopPrice >= tradePrice; });
    }

    void OrderBook::runCascade(ResultRecorder& recorder) {
        // Breadth first: each stop runs to completion, and what its trades trigger joins the
        // back of the queue. If anything throws, the guard in the command discards the stops
        // still queued, which have already left the pending set.
        while(triggeredStops && !triggeredStops->empty()) {
            const OrderRequest stop = triggeredStops->front().request;
            const Price tradePrice = triggeredStops->front().triggeredAt;
            // Room for its StopTriggered first: if that fails, this stop has not run.
            recorder.makeRoom(0, 1);
            recorder.triggered(OrderAccepted{stop.id, stop.side, stop.price, stop.quantity}, stop.stopPrice, tradePrice);
            triggeredStops->pop_front();
            // It becomes its market or limit order, under its own time in force, owner, and mode,
            // and takes a fresh sequence number when it runs.
            Order incoming = incomingOrder(stop);
            runIncoming(incoming, Instructions{stop.timeInForce, stop.owner, stop.selfTradePrevention}, recorder);
        }
    }

    bool OrderBook::isPastTrigger(const OrderRequest& stop) const noexcept {
        if(!lastTradePrice) {
            return false;
        }
        return stop.side == Side::BUY ? stop.stopPrice <= *lastTradePrice : stop.stopPrice >= *lastTradePrice;
    }

    void OrderBook::submitStop(const OrderRequest& request, ResultRecorder& recorder) {
        if(isPastTrigger(request)) {
            // Its acceptance and expiry fit inline, so nothing here allocates or throws.
            recorder.expire(request.quantity);
            advanceSequenceNum();
            return;
        }
        // Numbered last: consumed only once the stop is stored, so a failed allocation leaves
        // no trace. Its result is its acceptance alone.
        storePendingStop(request);
        advanceSequenceNum();
    }

    void OrderBook::storePendingStop(const OrderRequest& stop) {
        // First, so a failure changes nothing: the queue a triggered stop will move to.
        if(!triggeredStops) {
            triggeredStops = std::make_unique<StopQueue>(PoolAllocator<PendingStop>(nodePool.get()));
        }
        auto store = [this, &stop](auto& levels) {
            const auto levelItr = levels.try_emplace(stop.stopPrice, stop.stopPrice, stop.side, nodePool.get()).first;
            StopLevel& level = levelItr->second;
            std::optional<StopQueue::iterator> position;
            try {
                level.stops.push_back(PendingStop{stop, nextSequenceNum});
                position = std::prev(level.stops.end());
                if(!pendingIndex.emplace(stop.id, StopLocation(stop.stopPrice, stop.side, *position, &level)).second) {
                    throw std::runtime_error("storing a stop whose id is already indexed error");
                }
            }
            catch(...) {
                // Unwind so a failed allocation leaves no unindexed stop and no empty level.
                if(position) {
                    level.stops.erase(*position);
                }
                if(level.stops.empty()) {
                    levels.erase(levelItr);
                }
                throw;
            }
        };
        if(stop.side == Side::BUY) {
            store(buyStops);
        }
        else {
            store(sellStops);
        }
    }

    void OrderBook::removePendingStop(PendingStopIndex::iterator entry) {
        const StopLocation location = entry->second;
        auto remove = [this, entry, &location](auto& levels) {
            StopLevel* const level = location.level;
            if(level == nullptr || level->stopPrice != location.tickPrice || level->side != location.side) {
                throw std::runtime_error("indexed stop's level handle does not match its entry error");
            }
            // Nothing below can throw.
            level->stops.erase(location.itr);
            pendingIndex.erase(entry);
            if(level->stops.empty()) {
                levels.erase(location.tickPrice);
            }
        };
        if(location.side == Side::BUY) {
            remove(buyStops);
        }
        else {
            remove(sellStops);
        }
    }

    std::size_t OrderBook::pendingStopCount() const {
        return pendingIndex.size();
    }

    std::optional<OrderRequest> OrderBook::findPendingStop(OrderId orderId) const {
        const auto entry = pendingIndex.find(orderId);
        if(entry == pendingIndex.end()) {
            return std::nullopt;
        }
        return entry->second.itr->request;
    }

    std::optional<Price> OrderBook::getLastTradePrice() const noexcept {
        return lastTradePrice;
    }

    std::size_t OrderBook::activeOrderCount() const {
        return orderIndex.size();
    }

    void OrderBook::reserve(std::size_t orders) {
        // No InvariantBoundary: the book's contents do not change. A rehash that throws
        // (other than from the hash, which cannot) leaves the index as it was.
        orderIndex.reserve(orders);
    }

    void OrderBook::reserve(const BookCapacity& capacity) {
        // No InvariantBoundary: the book's contents do not change.
        if(capacity.orders <= reservedCapacity.orders && capacity.stops <= reservedCapacity.stops &&
           capacity.levels <= reservedCapacity.levels) {
            return;
        }
        const BookCapacity wanted{std::max(capacity.orders, reservedCapacity.orders),
                                  std::max(capacity.stops, reservedCapacity.stops),
                                  std::max(capacity.levels, reservedCapacity.levels)};
        // Whatever the pool takes in here is not a growth, even if a later step throws.
        const std::uint64_t chunksBefore = nodePool ? nodePool->chunksTaken() : 0;
        const auto countChunks = [this, chunksBefore] {
            reservedChunks += nodePool ? nodePool->chunksTaken() - chunksBefore : 0;
        };
        try {
            orderIndex.reserve(wanted.orders);
            pendingIndex.reserve(wanted.stops);
            bids.reserve(wanted.levels);
            asks.reserve(wanted.levels);
            buyStops.reserve(wanted.levels);
            sellStops.reserve(wanted.levels);
            if(wanted.stops > 0 && !triggeredStops) {
                triggeredStops = std::make_unique<StopQueue>(PoolAllocator<PendingStop>(nodePool.get()));
            }
            warmPool(wanted);
        }
        catch(...) {
            countChunks();
            throw;
        }
        countChunks();
        reservedCapacity = wanted;
    }

    void OrderBook::warmPool(const BookCapacity& wanted) {
        NodePool* const pool = nodePool.get();
        if(pool == nullptr) {
            return;  // a moved-from book: everything it makes comes from the heap
        }
        // Everything the book may hold at once, built together, so the pool ends with room for
        // all of it (a list's sentinel can share a size class with its nodes). What rests already
        // holds its blocks, so only the rest is built. Prices fall, so each level goes at the
        // back of its array and the build stays linear.
        LevelArray<PriceLevel, std::less<Price>> levels(pool);
        LevelArray<StopLevel, std::less<Price>> stopLevels(pool);
        const std::size_t allLevels = wanted.levels * 2;
        const std::size_t heldLevels = std::min(allLevels, bids.size() + asks.size());
        const std::size_t heldStopLevels = std::min(allLevels, buyStops.size() + sellStops.size());
        levels.reserve(allLevels - heldLevels);
        stopLevels.reserve(allLevels - heldStopLevels);
        for(std::size_t level = allLevels - heldLevels; level > 0; --level) {
            const auto price = static_cast<Price>(level);
            levels.try_emplace(price, price, Side::BUY, pool);
        }
        for(std::size_t level = allLevels - heldStopLevels; level > 0; --level) {
            const auto price = static_cast<Price>(level);
            stopLevels.try_emplace(price, price, Side::BUY, pool);
        }
        OrderQueue orders{PoolAllocator<RestingOrder>(pool)};
        const RestingOrder restingNode(Order(1, 1, 1, 1, Side::BUY));
        for(std::size_t order = activeOrderCount(); order < wanted.orders; ++order) {
            orders.push_back(restingNode);
        }
        StopQueue stops{PoolAllocator<PendingStop>(pool)};
        for(std::size_t stop = pendingStopCount(); stop < wanted.stops; ++stop) {
            stops.push_back(PendingStop{OrderRequest{}, 0});
        }
    }

    std::uint64_t OrderBook::capacityGrowths() const noexcept {
        const std::uint64_t poolGrowths = nodePool ? nodePool->chunksTaken() - reservedChunks : 0;
        return orderIndex.growths() + pendingIndex.growths() + bids.growths() + asks.growths() + buyStops.growths() +
               sellStops.growths() + poolGrowths;
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
            const auto stopItr = pendingIndex.find(orderId);
            if(stopItr == pendingIndex.end()) {
                return CancellationResult::rejected(orderId, RejectionReason::ORDER_NOT_FOUND);
            }
            // A pending stop never trades, so it has its whole quantity left.
            CancellationResult result = CancellationResult::cancelled(orderId, stopItr->second.itr->request.quantity);
            removePendingStop(stopItr);
            return result;
        }
        // Built before anything is mutated, so nothing after the removal can throw. (Its one
        // event is held inline, so building it does not allocate either.)
        CancellationResult result = CancellationResult::cancelled(orderId, indexItr->second.itr->getRemainingQuantity());
        removeResting(indexItr);
        return result;
    }

    void OrderBook::removeResting(OrderIndex::iterator entry) {
        const OrderLocation location = entry->second;
        auto remove = [this, entry, &location](auto& levels) {
            // The entry's own level handle, not a search of the side. An indexed order always
            // has its level; if the handle does not name a level at the entry's price and side,
            // fail before mutating anything.
            PriceLevel* const level = &checkedLevel(location);

            // Nothing below can throw. Unindex straight after erasing, so the entry never
            // holds the erased element's dangling iterator.
            level->removeOrder(location.itr);
            orderIndex.erase(entry);
            if(level->isEmpty()) {
                levels.erase(location.tickPrice);
            }
        };

        if(location.side == Side::BUY) {
            remove(bids);
        }
        else {
            remove(asks);
        }
    }

    ConsumptionResult OrderBook::consumeLevel(Side side, Price price, Quantity quantity) {
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
            result.fills.reserve(level.getOrderCount());

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
            const unsigned int orderCount = itr->second.getOrderCount();
            topBid.orderCount = orderCount;
            totalActiveOrders += orderCount;
            topBid.totalQuantity = itr->second.getAggregateQuantity();
            bestBids.push_back(topBid);
        }

        unsigned int asksCount = 0;
        std::vector<PriceLevelSummary> bestAsks;
        for(auto itr = asks.begin(); itr != asks.end() && asksCount < depth; ++itr, ++asksCount) {
            PriceLevelSummary topAsk;
            topAsk.price = itr->first;
            const unsigned int orderCount = itr->second.getOrderCount();
            topAsk.orderCount = orderCount;
            totalActiveOrders += orderCount;
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