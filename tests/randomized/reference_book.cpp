#include "reference_book.hpp"

#include <algorithm>
#include <stdexcept>

namespace order_book::randomized {

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
    return found == orders.end() ? nullptr : &found->second;
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
            const Quantity open = orders.at(orderId).open;
            level.totalQuantity += open;
            level.orders.emplace_back(orderId, open);
        }
        described.push_back(std::move(level));
    }
    return described;
}

ExpectedOutcome ReferenceBook::apply(const Command& command) {
    return command.type == CommandType::SUBMIT ? submit(command) : cancel(command.orderId);
}

// The engine's documented order: malformed fields (ID, price, quantity, side, type), then an
// order already marked FILLED, then an ID that is resting.
std::optional<RejectionReason> ReferenceBook::rejectionFor(const Command& command) const {
    if (command.orderId == 0) {
        return RejectionReason::INVALID_ORDER_ID;
    }
    if (command.price == 0) {
        return RejectionReason::INVALID_PRICE;
    }
    if (command.quantity == 0) {
        return RejectionReason::INVALID_QUANTITY;
    }
    if (command.side != Side::BUY && command.side != Side::SELL) {
        return RejectionReason::INVALID_SIDE;
    }
    if (command.orderType != OrderType::LIMIT) {
        return RejectionReason::UNSUPPORTED_ORDER_TYPE;
    }
    if (command.markedFilled) {
        return RejectionReason::ORDER_NOT_ACTIVE;
    }
    if (resting.contains(command.orderId)) {
        return RejectionReason::DUPLICATE_ORDER_ID;
    }
    return std::nullopt;
}

ExpectedOutcome ReferenceBook::submit(const Command& command) {
    ExpectedOutcome outcome;
    if (const std::optional<RejectionReason> reason = rejectionFor(command)) {
        outcome.rejection = reason;
        outcome.events.push_back(OrderRejected{command.orderId, *reason});
        outcome.remaining = command.quantity;
        return outcome;
    }

    outcome.success = true;
    outcome.events.push_back(OrderAccepted{command.orderId, command.side, command.price, command.quantity});
    Quantity open = command.quantity;
    if (command.side == Side::BUY) {
        matchAgainst(asks, command, open, outcome.events);
    } else {
        matchAgainst(bids, command, open, outcome.events);
    }
    outcome.remaining = open;
    const bool traded = outcome.events.size() > 1;
    outcome.fillStatus = !traded ? FillStatus::NONE : open == 0 ? FillStatus::FILLED : FillStatus::PARTIAL;

    removed.erase(command.orderId);
    if (open > 0) {
        rest(ModelOrder{command.orderId, command.side, command.price, open});
    } else {
        removed.insert(command.orderId);
    }
    return outcome;
}

// Takes from the oldest order at the best opposing price while the incoming order still has
// quantity and its limit reaches that price, trading at the resting price.
template <typename Levels>
void ReferenceBook::matchAgainst(Levels& levels, const Command& incoming, Quantity& open,
                                 std::vector<BookEvent>& events) {
    while (open > 0 && !levels.empty()) {
        auto level = levels.begin();
        const Price price = level->first;
        const bool reaches = incoming.side == Side::BUY ? price <= incoming.price : price >= incoming.price;
        if (!reaches) {
            return;
        }
        ModelOrder& maker = orders.at(level->second.front());
        const Quantity traded = std::min(open, maker.open);
        events.push_back(TradeExecuted{incoming.orderId, maker.id, price, traded});
        open -= traded;
        maker.open -= traded;
        if (maker.open == 0) {
            const OrderId makerId = maker.id;
            level->second.pop_front();
            if (level->second.empty()) {
                levels.erase(level);
            }
            retire(makerId);
        }
    }
}

ExpectedOutcome ReferenceBook::cancel(OrderId orderId) {
    ExpectedOutcome outcome;
    const auto found = orders.find(orderId);
    if (found == orders.end()) {
        outcome.rejection = RejectionReason::ORDER_NOT_FOUND;
        outcome.events.push_back(CancelRejected{orderId, RejectionReason::ORDER_NOT_FOUND});
        return outcome;
    }

    const ModelOrder order = found->second;
    outcome.success = true;
    outcome.events.push_back(OrderCancelled{orderId, order.open});
    auto removeFrom = [&](auto& levels) {
        const auto level = levels.find(order.price);
        level->second.remove(orderId);
        if (level->second.empty()) {
            levels.erase(level);
        }
    };
    if (order.side == Side::BUY) {
        removeFrom(bids);
    } else {
        removeFrom(asks);
    }
    retire(orderId);
    return outcome;
}

void ReferenceBook::rest(const ModelOrder& order) {
    orders.emplace(order.id, order);
    resting.insert(order.id);
    if (order.side == Side::BUY) {
        bids[order.price].push_back(order.id);
    } else {
        asks[order.price].push_back(order.id);
    }
}

// Forgets a resting order the model has already taken off its level.
void ReferenceBook::retire(OrderId orderId) {
    orders.erase(orderId);
    resting.erase(orderId);
    removed.insert(orderId);
}

}  // namespace order_book::randomized
