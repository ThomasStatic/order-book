#include "reference_book.hpp"

#include <algorithm>

namespace order_book::reference {

void IdPool::insert(OrderId orderId) {
    if (contains(orderId)) {
        return;
    }
    positions.emplace(orderId, ids.size());
    ids.push_back(orderId);
}

void IdPool::erase(OrderId orderId) {
    const auto found = positions.find(orderId);
    if (found == positions.end()) {
        return;
    }
    const std::size_t position = found->second;
    positions.erase(found);
    if (position + 1 != ids.size()) {
        ids[position] = ids.back();
        positions[ids[position]] = position;
    }
    ids.pop_back();
}

const ModelOrder* ReferenceBook::find(OrderId orderId) const {
    const auto found = orders.find(orderId);
    return found == orders.end() ? nullptr : &found->second.order;
}

const OrderRequest* ReferenceBook::findPendingStop(OrderId orderId) const {
    const auto found = stops.find(orderId);
    return found == stops.end() ? nullptr : &found->second.request;
}

std::optional<Price> ReferenceBook::best(Side side) const {
    if (side == Side::BUY) {
        return bids.empty() ? std::nullopt : std::optional<Price>(bids.begin()->first);
    }
    return asks.empty() ? std::nullopt : std::optional<Price>(asks.begin()->first);
}

std::vector<ModelLevel> ReferenceBook::levels(Side side) const {
    return side == Side::BUY ? describe(bids) : describe(asks);
}

template <typename Levels>
std::vector<ModelLevel> ReferenceBook::describe(const Levels& levels) const {
    std::vector<ModelLevel> described;
    for (const auto& [price, queue] : levels) {
        ModelLevel level{price, 0, {}};
        for (OrderId orderId : queue) {
            const Quantity open = orders.at(orderId).order.open;
            level.totalQuantity += open;
            level.orders.emplace_back(orderId, open);
        }
        described.push_back(std::move(level));
    }
    return described;
}

namespace {

bool isStop(OrderType type) {
    return type == OrderType::STOP || type == OrderType::STOP_LIMIT;
}

// MARKET and STOP orders carry no price.
bool priceless(OrderType type) {
    return type == OrderType::MARKET || type == OrderType::STOP;
}

bool knownType(OrderType type) {
    return type == OrderType::LIMIT || type == OrderType::MARKET || isStop(type);
}

bool knownTimeInForce(TimeInForce timeInForce) {
    return timeInForce == TimeInForce::GTC || timeInForce == TimeInForce::IOC || timeInForce == TimeInForce::FOK;
}

bool knownMode(SelfTradePrevention mode) {
    return mode == SelfTradePrevention::CANCEL_RESTING || mode == SelfTradePrevention::CANCEL_INCOMING ||
           mode == SelfTradePrevention::CANCEL_BOTH;
}

// The checks every submission shares, in the documented order: a zero ID, a zero price (none
// is needed on a MARKET or STOP order), a zero quantity, then a side that is neither BUY nor SELL.
std::optional<RejectionReason> fieldRejection(const OrderRequest& request, bool priceRequired) {
    if (request.id == 0) {
        return RejectionReason::INVALID_ORDER_ID;
    }
    if (priceRequired && request.price == 0) {
        return RejectionReason::INVALID_PRICE;
    }
    if (request.quantity == 0) {
        return RejectionReason::INVALID_QUANTITY;
    }
    if (request.side != Side::BUY && request.side != Side::SELL) {
        return RejectionReason::INVALID_SIDE;
    }
    return std::nullopt;
}

}  // namespace

// The request table, in order; the live-ID check comes after it.
std::optional<RejectionReason> ReferenceBook::requestRejection(const OrderRequest& request) const {
    if (const std::optional<RejectionReason> reason = fieldRejection(request, !priceless(request.type))) {
        return reason;
    }
    if (!knownType(request.type)) {
        return RejectionReason::UNSUPPORTED_ORDER_TYPE;
    }
    if (priceless(request.type) && request.price != 0) {
        return RejectionReason::PRICE_NOT_ALLOWED;
    }
    if (!knownTimeInForce(request.timeInForce)) {
        return RejectionReason::INVALID_TIME_IN_FORCE;
    }
    if (priceless(request.type) && request.timeInForce == TimeInForce::GTC) {
        return RejectionReason::TIME_IN_FORCE_NOT_ALLOWED;
    }
    if (isStop(request.type) && request.stopPrice == 0) {
        return RejectionReason::INVALID_STOP_PRICE;
    }
    if (!isStop(request.type) && request.stopPrice != 0) {
        return RejectionReason::STOP_PRICE_NOT_ALLOWED;
    }
    if (request.postOnly && (request.type != OrderType::LIMIT || request.timeInForce != TimeInForce::GTC)) {
        return RejectionReason::POST_ONLY_NOT_ALLOWED;
    }
    if (!knownMode(request.selfTradePrevention)) {
        return RejectionReason::INVALID_SELF_TRADE_PREVENTION;
    }
    if (resting.contains(request.id) || pending.contains(request.id)) {
        return RejectionReason::DUPLICATE_ORDER_ID;
    }
    return std::nullopt;
}

ExpectedOutcome ReferenceBook::reject(OrderId orderId, Quantity quantity, RejectionReason reason) {
    ExpectedOutcome outcome;
    outcome.rejection = reason;
    outcome.events.push_back(OrderRejected{orderId, reason});
    outcome.remaining = quantity;
    ++seen.submissionsRejected;
    return outcome;
}

// Accepted and wholly expired, without trading: a post-only order that would take, or a stop
// already past its trigger.
ExpectedOutcome ReferenceBook::expireWhole(const OrderRequest& request) {
    ExpectedOutcome outcome;
    outcome.success = true;
    ++seen.submissionsAccepted;
    ++seen.expired;
    outcome.events = {OrderAccepted{request.id, request.side, request.price, request.quantity},
                      OrderExpired{request.id, request.quantity}};
    outcome.remaining = request.quantity;
    removed.erase(request.id);
    removed.insert(request.id);
    return outcome;
}

ExpectedOutcome ReferenceBook::submit(const OrderRequest& request) {
    if (const std::optional<RejectionReason> reason = requestRejection(request)) {
        return reject(request.id, request.quantity, *reason);
    }
    if (isStop(request.type)) {
        // Past its trigger: a buy stop at or below the last trade price, a sell stop at or
        // above it. Before the first trade, none is.
        const bool past = lastTrade && (request.side == Side::BUY ? request.stopPrice <= *lastTrade
                                                                  : request.stopPrice >= *lastTrade);
        if (past) {
            return expireWhole(request);
        }
        ExpectedOutcome outcome;
        outcome.success = true;
        ++seen.submissionsAccepted;
        outcome.events.push_back(OrderAccepted{request.id, request.side, request.price, request.quantity});
        outcome.remaining = request.quantity;
        removed.erase(request.id);
        pend(request);
        return outcome;
    }
    if (request.postOnly) {
        // Decided on price alone: at or through the opposing best would take, so it expires.
        const std::optional<Price> opposing = best(request.side == Side::BUY ? Side::SELL : Side::BUY);
        const bool takes = opposing && (request.side == Side::BUY ? request.price >= *opposing : request.price <= *opposing);
        if (takes) {
            return expireWhole(request);
        }
    }
    ExpectedOutcome outcome;
    outcome.success = true;
    ++seen.submissionsAccepted;
    outcome.events.push_back(OrderAccepted{request.id, request.side, request.price, request.quantity});
    const Quantity filled = runOrder(request, outcome.events);
    outcome.remaining = request.quantity - filled;
    outcome.fillStatus = filled == 0 ? FillStatus::NONE : outcome.remaining == 0 ? FillStatus::FILLED : FillStatus::PARTIAL;
    runCascade(outcome.events);
    return outcome;
}

ExpectedOutcome ReferenceBook::submitOrder(const OrderRequest& fields, bool markedFilled) {
    std::optional<RejectionReason> reason = fieldRejection(fields, true);
    if (!reason && fields.type != OrderType::LIMIT) {
        reason = RejectionReason::UNSUPPORTED_ORDER_TYPE;  // only a LIMIT order can rest
    }
    if (!reason && markedFilled) {
        reason = RejectionReason::ORDER_NOT_ACTIVE;
    }
    if (!reason && (resting.contains(fields.id) || pending.contains(fields.id))) {
        reason = RejectionReason::DUPLICATE_ORDER_ID;
    }
    if (reason) {
        return reject(fields.id, fields.quantity, *reason);
    }
    OrderRequest limit = fields;
    limit.timeInForce = TimeInForce::GTC;
    ExpectedOutcome outcome;
    outcome.success = true;
    ++seen.submissionsAccepted;
    outcome.events.push_back(OrderAccepted{fields.id, fields.side, fields.price, fields.quantity});
    const Quantity filled = runOrder(limit, outcome.events);
    outcome.remaining = fields.quantity - filled;
    outcome.fillStatus = filled == 0 ? FillStatus::NONE : outcome.remaining == 0 ? FillStatus::FILLED : FillStatus::PARTIAL;
    runCascade(outcome.events);
    return outcome;
}

// Matches the order by its plan (a FOK only if the plan fills it), then rests what is left if
// it is a GTC limit not stopped by self-trade prevention, and otherwise expires it.
Quantity ReferenceBook::runOrder(const OrderRequest& order, std::vector<BookEvent>& events) {
    const Price limit = priceless(order.type) ? 0 : order.price;
    MatchPlan matchPlan = plan(order.side, limit, order.quantity, order.owner, order.selfTradePrevention);
    if (order.timeInForce == TimeInForce::FOK && matchPlan.filled < order.quantity) {
        matchPlan = MatchPlan{};  // discarded, preventions included: it cancels nothing
    }
    applyPlan(matchPlan, order.id, events);
    const Quantity open = order.quantity - matchPlan.filled;
    removed.erase(order.id);
    const bool rests = order.timeInForce == TimeInForce::GTC && !matchPlan.stopped && !priceless(order.type);
    if (open > 0 && rests) {
        rest(ModelOrder{order.id, order.side, order.price, open, order.owner});
    } else {
        if (open > 0) {
            events.push_back(OrderExpired{order.id, open});
            ++seen.expired;
        }
        removed.insert(order.id);
    }
    return matchPlan.filled;
}

// Breadth first: each triggered stop runs as its market or limit order, after everything
// triggered before it; what its trades trigger joins the back.
void ReferenceBook::runCascade(std::vector<BookEvent>& events) {
    while (!triggered.empty()) {
        const Triggered next = triggered.front();
        triggered.pop_front();
        ++seen.stopsTriggered;
        events.push_back(StopTriggered{next.request.id, next.request.stopPrice, next.tradePrice});
        runOrder(next.request, events);
    }
}

MatchPlan ReferenceBook::plan(Side side, Price limitPrice, Quantity quantity, OwnerId owner,
                              SelfTradePrevention mode) const {
    MatchPlan matchPlan;
    if (side == Side::BUY) {
        planAgainst(asks, side, limitPrice, quantity, owner, mode, matchPlan);
    } else {
        planAgainst(bids, side, limitPrice, quantity, owner, mode, matchPlan);
    }
    return matchPlan;
}

// Walks the opposing levels best first and each queue oldest first, taking from each maker
// the smaller of what the incoming order still wants and what the maker has open, while the
// incoming limit reaches the level's price. A maker with the incoming order's nonzero owner is
// a prevented self-trade instead: under CANCEL_RESTING the walk goes on past it; under the
// other modes it stops there. Each maker appears at most once, so what a step takes from it
// never has to be remembered for a later step.
template <typename Levels>
void ReferenceBook::planAgainst(const Levels& levels, Side side, Price limitPrice, Quantity quantity, OwnerId owner,
                                SelfTradePrevention mode, MatchPlan& matchPlan) const {
    for (const auto& [price, queue] : levels) {
        const bool reaches = limitPrice == 0 || (side == Side::BUY ? price <= limitPrice : price >= limitPrice);
        if (!reaches) {
            return;
        }
        for (OrderId makerId : queue) {
            if (matchPlan.filled == quantity) {
                return;
            }
            const ModelOrder& maker = orders.at(makerId).order;
            if (owner != 0 && maker.owner == owner) {
                const bool removes = mode != SelfTradePrevention::CANCEL_INCOMING;
                matchPlan.steps.push_back(PlanStep{makerId, price, removes ? maker.open : 0, true});
                if (mode != SelfTradePrevention::CANCEL_RESTING) {
                    matchPlan.stopped = true;
                    return;
                }
                continue;
            }
            const Quantity traded = std::min(quantity - matchPlan.filled, maker.open);
            matchPlan.steps.push_back(PlanStep{makerId, price, traded});
            matchPlan.filled += traded;
        }
    }
}

void ReferenceBook::applyPlan(const MatchPlan& matchPlan, OrderId incomingId, std::vector<BookEvent>& events) {
    for (const PlanStep& step : matchPlan.steps) {
        if (step.prevented) {
            events.push_back(SelfTradePrevented{incomingId, step.maker, step.price, step.quantity});
            ++seen.preventions;
            if (step.quantity > 0) {
                remove(step.maker);
            }
            continue;
        }
        events.push_back(TradeExecuted{incomingId, step.maker, step.price, step.quantity});
        ++seen.trades;
        Entry& maker = orders.at(step.maker);
        maker.order.open -= step.quantity;
        if (maker.order.open == 0) {
            remove(step.maker);
        }
        lastTrade = step.price;
        triggerAt(step.price);
    }
}

// Every pending stop a trade at this price reaches: buy stops at or below it, sell stops at or
// above it, nearest first, in acceptance order within a stop price.
void ReferenceBook::triggerAt(Price tradePrice) {
    auto take = [this, tradePrice](auto& levels, auto reached) {
        while (!levels.empty() && reached(levels.begin()->first)) {
            for (OrderId stopId : levels.begin()->second) {
                triggered.push_back(Triggered{stops.at(stopId).request, tradePrice});
                stops.erase(stopId);
                pending.erase(stopId);
            }
            levels.erase(levels.begin());
        }
    };
    take(buyStops, [tradePrice](Price stopPrice) { return stopPrice <= tradePrice; });
    take(sellStops, [tradePrice](Price stopPrice) { return stopPrice >= tradePrice; });
}

ExpectedOutcome ReferenceBook::cancel(OrderId orderId) {
    ExpectedOutcome outcome;
    if (const auto found = orders.find(orderId); found != orders.end()) {
        outcome.success = true;
        outcome.events.push_back(OrderCancelled{orderId, found->second.order.open});
        ++seen.cancelled;
        remove(orderId);
        return outcome;
    }
    if (const auto stop = stops.find(orderId); stop != stops.end()) {
        outcome.success = true;
        outcome.events.push_back(OrderCancelled{orderId, stop->second.request.quantity});
        ++seen.cancelled;
        unpend(orderId);
        return outcome;
    }
    outcome.rejection = RejectionReason::ORDER_NOT_FOUND;
    outcome.events.push_back(CancelRejected{orderId, RejectionReason::ORDER_NOT_FOUND});
    ++seen.cancelsRejected;
    return outcome;
}

void ReferenceBook::rest(const ModelOrder& order) {
    Queue& queue = order.side == Side::BUY ? bids[order.price] : asks[order.price];
    queue.push_back(order.id);
    orders.emplace(order.id, Entry{order, std::prev(queue.end())});
    resting.insert(order.id);
}

void ReferenceBook::pend(const OrderRequest& stop) {
    Queue& queue = stop.side == Side::BUY ? buyStops[stop.stopPrice] : sellStops[stop.stopPrice];
    queue.push_back(stop.id);
    stops.emplace(stop.id, StopEntry{stop, std::prev(queue.end())});
    pending.insert(stop.id);
}

void ReferenceBook::remove(OrderId orderId) {
    const auto found = orders.find(orderId);
    const ModelOrder order = found->second.order;
    auto removeFrom = [&](auto& levels) {
        const auto level = levels.find(order.price);
        level->second.erase(found->second.position);
        if (level->second.empty()) {
            levels.erase(level);
        }
    };
    if (order.side == Side::BUY) {
        removeFrom(bids);
    } else {
        removeFrom(asks);
    }
    orders.erase(found);
    resting.erase(orderId);
    removed.insert(orderId);
}

void ReferenceBook::unpend(OrderId orderId) {
    const auto found = stops.find(orderId);
    const OrderRequest stop = found->second.request;
    auto removeFrom = [&](auto& levels) {
        const auto level = levels.find(stop.stopPrice);
        level->second.erase(found->second.position);
        if (level->second.empty()) {
            levels.erase(level);
        }
    };
    if (stop.side == Side::BUY) {
        removeFrom(buyStops);
    } else {
        removeFrom(sellStops);
    }
    stops.erase(found);
    pending.erase(orderId);
    removed.insert(orderId);
}

}  // namespace order_book::reference
