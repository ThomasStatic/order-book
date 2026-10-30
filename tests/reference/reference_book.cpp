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

ExpectedOutcome ReferenceBook::reject(OrderId orderId, Quantity quantity, RejectionReason reason) {
    ExpectedOutcome outcome;
    outcome.rejection = reason;
    outcome.events.push_back(OrderRejected{orderId, reason});
    outcome.remaining = quantity;
    ++seen.submissionsRejected;
    return outcome;
}

namespace {

// The checks every submission shares, in the documented order: a zero ID, a zero price (none
// is needed on a market order), a zero quantity, then a side that is neither BUY nor SELL.
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

ExpectedOutcome ReferenceBook::submit(const OrderRequest& request) {
    const bool market = request.type == OrderType::MARKET;
    std::optional<RejectionReason> reason = fieldRejection(request, !market);
    if (!reason && request.type != OrderType::LIMIT && !market) {
        reason = RejectionReason::UNSUPPORTED_ORDER_TYPE;
    }
    if (!reason && market && request.price != 0) {
        reason = RejectionReason::PRICE_NOT_ALLOWED;
    }
    if (!reason && request.timeInForce != TimeInForce::GTC && request.timeInForce != TimeInForce::IOC &&
        request.timeInForce != TimeInForce::FOK) {
        reason = RejectionReason::INVALID_TIME_IN_FORCE;
    }
    if (!reason && market && request.timeInForce == TimeInForce::GTC) {
        reason = RejectionReason::TIME_IN_FORCE_NOT_ALLOWED;
    }
    if (!reason && resting.contains(request.id)) {
        reason = RejectionReason::DUPLICATE_ORDER_ID;
    }
    if (reason) {
        return reject(request.id, request.quantity, *reason);
    }
    return accept(request, request.timeInForce);
}

ExpectedOutcome ReferenceBook::submitOrder(const OrderRequest& fields, bool markedFilled) {
    std::optional<RejectionReason> reason = fieldRejection(fields, true);
    if (!reason && fields.type != OrderType::LIMIT) {
        reason = RejectionReason::UNSUPPORTED_ORDER_TYPE;  // only a LIMIT order can rest
    }
    if (!reason && markedFilled) {
        reason = RejectionReason::ORDER_NOT_ACTIVE;
    }
    if (!reason && resting.contains(fields.id)) {
        reason = RejectionReason::DUPLICATE_ORDER_ID;
    }
    return reason ? reject(fields.id, fields.quantity, *reason) : accept(fields, TimeInForce::GTC);
}

// A well-formed limit order: match it by its plan, then rest whatever is left (GTC), or expire
// it (IOC). A FOK whose plan does not fill it discards the plan and expires whole.
ExpectedOutcome ReferenceBook::accept(const OrderRequest& request, TimeInForce timeInForce) {
    ExpectedOutcome outcome;
    outcome.success = true;
    ++seen.submissionsAccepted;
    outcome.events.push_back(OrderAccepted{request.id, request.side, request.price, request.quantity});

    MatchPlan matchPlan = plan(request.side, request.price, request.quantity);
    if (timeInForce == TimeInForce::FOK && matchPlan.filled < request.quantity) {
        matchPlan = MatchPlan{};
    }
    applyPlan(matchPlan, request.id, outcome.events);
    const bool restsRemainder = timeInForce == TimeInForce::GTC;
    const Quantity open = request.quantity - matchPlan.filled;
    outcome.remaining = open;
    outcome.fillStatus = matchPlan.steps.empty() ? FillStatus::NONE : open == 0 ? FillStatus::FILLED : FillStatus::PARTIAL;

    removed.erase(request.id);
    if (open > 0 && restsRemainder) {
        rest(ModelOrder{request.id, request.side, request.price, open});
    } else {
        if (open > 0) {
            outcome.events.push_back(OrderExpired{request.id, open});
            ++seen.expired;
        }
        removed.insert(request.id);
    }
    return outcome;
}

MatchPlan ReferenceBook::plan(Side side, Price limitPrice, Quantity quantity) const {
    MatchPlan matchPlan;
    if (side == Side::BUY) {
        planAgainst(asks, side, limitPrice, quantity, matchPlan);
    } else {
        planAgainst(bids, side, limitPrice, quantity, matchPlan);
    }
    return matchPlan;
}

// Walks the opposing levels best first and each queue oldest first, taking from each maker
// the smaller of what the incoming order still wants and what the maker has open, while the
// incoming limit reaches the level's price. Each maker appears at most once, so what a step
// takes from it never has to be remembered for a later step.
template <typename Levels>
void ReferenceBook::planAgainst(const Levels& levels, Side side, Price limitPrice, Quantity quantity,
                                MatchPlan& matchPlan) const {
    for (const auto& [price, queue] : levels) {
        const bool reaches = limitPrice == 0 || (side == Side::BUY ? price <= limitPrice : price >= limitPrice);
        if (!reaches) {
            return;
        }
        for (OrderId makerId : queue) {
            if (matchPlan.filled == quantity) {
                return;
            }
            const Quantity traded = std::min(quantity - matchPlan.filled, orders.at(makerId).order.open);
            matchPlan.steps.push_back(PlannedTrade{makerId, price, traded});
            matchPlan.filled += traded;
        }
    }
}

void ReferenceBook::applyPlan(const MatchPlan& matchPlan, OrderId incomingId, std::vector<BookEvent>& events) {
    for (const PlannedTrade& step : matchPlan.steps) {
        events.push_back(TradeExecuted{incomingId, step.maker, step.price, step.quantity});
        lastTrade = step.price;
        ++seen.trades;
        Entry& maker = orders.at(step.maker);
        maker.order.open -= step.quantity;
        if (maker.order.open == 0) {
            remove(step.maker);
        }
    }
}

ExpectedOutcome ReferenceBook::cancel(OrderId orderId) {
    ExpectedOutcome outcome;
    const auto found = orders.find(orderId);
    if (found == orders.end()) {
        outcome.rejection = RejectionReason::ORDER_NOT_FOUND;
        outcome.events.push_back(CancelRejected{orderId, RejectionReason::ORDER_NOT_FOUND});
        ++seen.cancelsRejected;
        return outcome;
    }
    outcome.success = true;
    outcome.events.push_back(OrderCancelled{orderId, found->second.order.open});
    ++seen.cancelled;
    remove(orderId);
    return outcome;
}

void ReferenceBook::rest(const ModelOrder& order) {
    Queue& queue = order.side == Side::BUY ? bids[order.price] : asks[order.price];
    queue.push_back(order.id);
    orders.emplace(order.id, Entry{order, std::prev(queue.end())});
    resting.insert(order.id);
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

}  // namespace order_book::reference
