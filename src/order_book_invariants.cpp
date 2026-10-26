#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>

#include "order_book/order_book.hpp"

namespace order_book {
    namespace {
        const char* sideName(Side side) {
            if(side == Side::BUY) {
                return "BUY";
            }
            return side == Side::SELL ? "SELL" : "INVALID";
        }
    }  // namespace

    std::string formatInvariantResult(const InvariantResult& result) {
        if(result.isValid()) {
            return "valid";
        }
        std::string line = invariantViolationName(*result.violation);
        if(result.side) {
            line += std::string(" side=") + sideName(*result.side);
        }
        if(result.price) {
            line += " price=" + std::to_string(*result.price);
        }
        if(result.orderId) {
            line += " order=" + std::to_string(*result.orderId);
        }
        if(result.expected) {
            line += " expected=" + std::to_string(*result.expected);
        }
        if(result.actual) {
            line += " actual=" + std::to_string(*result.actual);
        }
        if(*result.detail != '\0') {
            line += std::string(" (") + result.detail + ")";
        }
        return line;
    }

    std::uint64_t OrderBook::sequenceAge(SequenceNum sequenceNum) const noexcept {
        // Valid numbers run from 1 to the maximum, so the counter cycles through that many.
        constexpr std::uint64_t cycle = std::numeric_limits<SequenceNum>::max();
        return (static_cast<std::uint64_t>(nextSequenceNum) + cycle - sequenceNum) % cycle;
    }

    bool OrderBook::idRestsElsewhere(const Order& order) const noexcept {
        const auto holdsAnother = [&order](const auto& levels) {
            for(const auto& entry : levels) {
                for(const Order& other : entry.second.orders) {
                    if(&other != &order && other.getOrderId() == order.getOrderId()) {
                        return true;
                    }
                }
            }
            return false;
        };
        return holdsAnother(bids) || holdsAnother(asks);
    }

    template <typename Levels>
    InvariantResult OrderBook::checkLevels(const Levels& levels, Side side, std::size_t& restingOrders) const noexcept {
        using enum InvariantViolation;
        const InvariantViolation misordered = side == Side::BUY ? INVALID_BID_ORDERING : INVALID_ASK_ORDERING;

        std::optional<Price> previousPrice;
        for(const auto& [price, level] : levels) {
            // std::map keeps its keys in comparator order, so this guards against a change of
            // container or comparator rather than against anything a command can do.
            if(previousPrice && !levels.key_comp()(*previousPrice, price)) {
                return {.violation = misordered, .side = side, .price = price, .expected = *previousPrice,
                        .actual = price, .detail = "levels should run from the best price outward, one per price"};
            }
            previousPrice = price;

            if(level.tickPrice != price) {
                return {.violation = MISPLACED_LEVEL, .side = side, .price = price, .expected = price,
                        .actual = level.tickPrice, .detail = "a level's own price should be the one it is stored under"};
            }
            if(level.side != side) {
                return {.violation = MISPLACED_LEVEL, .side = side, .price = price,
                        .detail = "a level's own side should be the side of the book holding it"};
            }
            if(level.orders.empty()) {
                return {.violation = EMPTY_PRICE_LEVEL, .side = side, .price = price,
                        .detail = "a level should be erased once its last order leaves"};
            }

            std::uint64_t remainingSum = 0;
            const Order* ahead = nullptr;
            for(const Order& order : level.orders) {
                ++restingOrders;
                const OrderId orderId = order.getOrderId();

                if(order.getRemainingQuantity() == 0) {
                    return {.violation = ZERO_QUANTITY_ORDER, .side = side, .price = price, .orderId = orderId,
                            .detail = "a fully filled order should leave its level"};
                }
                if(const std::optional<RejectionReason> reason = order.malformedReason()) {
                    return {.violation = MALFORMED_ORDER, .side = side, .price = price, .orderId = orderId,
                            .detail = rejectionMessage(*reason)};
                }
                if(!order.orderIsActive()) {
                    return {.violation = MALFORMED_ORDER, .side = side, .price = price, .orderId = orderId,
                            .detail = "a resting order should not be marked FILLED"};
                }
                if(order.getSequenceNum() == 0) {
                    return {.violation = MALFORMED_ORDER, .side = side, .price = price, .orderId = orderId,
                            .detail = "a resting order should have a sequence number"};
                }
                if(order.getSide() != side) {
                    return {.violation = MISPLACED_ORDER, .side = side, .price = price, .orderId = orderId,
                            .detail = "an order's side should be its level's"};
                }
                if(order.getPriceTick() != price) {
                    return {.violation = MISPLACED_ORDER, .side = side, .price = price, .orderId = orderId,
                            .expected = price, .actual = order.getPriceTick(),
                            .detail = "an order's price should be its level's"};
                }
                // An order accepted later was issued fewer numbers ago, even across a wrap.
                if(ahead != nullptr && sequenceAge(order.getSequenceNum()) >= sequenceAge(ahead->getSequenceNum())) {
                    return {.violation = INVALID_FIFO_SEQUENCE, .side = side, .price = price, .orderId = orderId,
                            .expected = ahead->getSequenceNum(), .actual = order.getSequenceNum(),
                            .detail = "each order should have been accepted after the one ahead of it"};
                }
                ahead = &order;
                remainingSum += order.getRemainingQuantity();
            }

            if(remainingSum != level.quantity) {
                return {.violation = AGGREGATE_QUANTITY_MISMATCH, .side = side, .price = price,
                        .expected = remainingSum, .actual = level.quantity,
                        .detail = "a level's cached quantity should equal its orders' remaining sum"};
            }
        }
        return {};
    }

    template <typename Levels>
    InvariantResult OrderBook::checkIndexedLevels(const Levels& levels, Side side) const noexcept {
        using enum InvariantViolation;
        for(const auto& [price, level] : levels) {
            for(auto orderItr = level.orders.begin(); orderItr != level.orders.end(); ++orderItr) {
                const OrderId orderId = orderItr->getOrderId();
                const auto entry = orderIndex.find(orderId);
                if(entry == orderIndex.end()) {
                    return {.violation = MISSING_INDEX_ENTRY, .side = side, .price = price, .orderId = orderId,
                            .detail = "every resting order should be indexed"};
                }

                // The iterators are compared only once the entry names this level, so both
                // belong to the same list. The level handle is compared, never followed.
                const OrderLocation& location = entry->second;
                if(location.side == side && location.tickPrice == price && location.level == &level
                   && location.itr == orderItr) {
                    continue;
                }
                if(idRestsElsewhere(*orderItr)) {
                    return {.violation = DUPLICATE_ORDER_ID, .side = side, .price = price, .orderId = orderId,
                            .detail = "another resting order has this ID"};
                }
                if(location.side != side) {
                    return {.violation = INVALID_INDEX_ENTRY, .side = side, .price = price, .orderId = orderId,
                            .detail = "an index entry should record its order's side"};
                }
                if(location.tickPrice != price) {
                    return {.violation = INVALID_INDEX_ENTRY, .side = side, .price = price, .orderId = orderId,
                            .expected = price, .actual = location.tickPrice,
                            .detail = "an index entry should record its order's price"};
                }
                if(location.level != &level) {
                    return {.violation = INVALID_INDEX_ENTRY, .side = side, .price = price, .orderId = orderId,
                            .detail = "an index entry should point at its order's level"};
                }
                return {.violation = INVALID_INDEX_ENTRY, .side = side, .price = price, .orderId = orderId,
                        .detail = "an index entry should point at its own order"};
            }
        }
        return {};
    }

    InvariantResult OrderBook::findStaleIndexEntry(std::size_t restingOrders) const noexcept {
        // Every resting order has its own entry by now, so an extra one is told apart by its
        // key and recorded place alone. Its iterator is left alone, since it may point at an
        // order that has been erased.
        for(const auto& [orderId, location] : orderIndex) {
            if(!isValidSide(location.side)) {
                return {.violation = InvariantViolation::INVALID_INDEX_ENTRY, .price = location.tickPrice,
                        .orderId = orderId, .detail = "an index entry should record BUY or SELL"};
            }
            const auto whyStale = [key = orderId, &location](const auto& levels) -> const char* {
                const auto levelItr = levels.find(location.tickPrice);
                if(levelItr == levels.end()) {
                    return "an index entry should refer to a level in the book";
                }
                for(const Order& order : levelItr->second.orders) {
                    if(order.getOrderId() == key) {
                        return nullptr;
                    }
                }
                return "an index entry should refer to a resting order";
            };
            if(const char* detail = location.side == Side::BUY ? whyStale(bids) : whyStale(asks)) {
                return {.violation = InvariantViolation::INVALID_INDEX_ENTRY, .side = location.side,
                        .price = location.tickPrice, .orderId = orderId, .detail = detail};
            }
        }
        // Unreachable while the earlier checks hold, but still report the mismatch.
        return {.violation = InvariantViolation::INVALID_INDEX_ENTRY, .expected = restingOrders,
                .actual = orderIndex.size(), .detail = "the index should hold one entry per resting order"};
    }

    InvariantResult OrderBook::checkInvariants() const noexcept {
        // 1 and 2: each side's levels and orders on their own.
        std::size_t restingOrders = 0;
        if(InvariantResult result = checkLevels(bids, Side::BUY, restingOrders); !result.isValid()) {
            return result;
        }
        if(InvariantResult result = checkLevels(asks, Side::SELL, restingOrders); !result.isValid()) {
            return result;
        }

        // 3: the index against the orders. Once every resting order has an entry of its own,
        // a larger index can only hold entries that refer to no resting order.
        if(InvariantResult result = checkIndexedLevels(bids, Side::BUY); !result.isValid()) {
            return result;
        }
        if(InvariantResult result = checkIndexedLevels(asks, Side::SELL); !result.isValid()) {
            return result;
        }
        if(orderIndex.size() != restingOrders) {
            return findStaleIndexEntry(restingOrders);
        }

        // 4: the top of the book.
        if(!bids.empty() && !asks.empty() && bids.begin()->first >= asks.begin()->first) {
            return {.violation = InvariantViolation::CROSSED_BOOK, .expected = asks.begin()->first,
                    .actual = bids.begin()->first, .detail = "the best bid should be strictly below the best ask"};
        }
        return {};
    }

    void OrderBook::enforceInvariants(const char* command) const noexcept {
        const InvariantResult result = checkInvariants();
        if(result.isValid()) {
            return;
        }
        // Formatting allocates; if that fails, still name the violation.
        try {
            const std::string line = formatInvariantResult(result);
            std::fprintf(stderr, "order book invariant violated after %s: %s\n", command, line.c_str());
        }
        catch(...) {
            std::fprintf(stderr, "order book invariant violated after %s: %s\n", command,
                         invariantViolationName(*result.violation));
        }
        std::fflush(stderr);
        std::abort();
    }
}  // namespace order_book
