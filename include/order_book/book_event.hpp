#pragma once

#include <variant>

#include "rejection_reason.hpp"
#include "side.hpp"
#include "types.hpp"

namespace order_book {

    // Lifecycle events record what a command did to the book. They are domain output, held
    // by the command's result in the order they happened, and never printed by the engine.

    // The order passed validation and is about to be matched; quantity is what it had left
    // to trade on acceptance, before any matching (its whole size, for a fresh order). price
    // is its limit price, or 0 for an order with none (a market or STOP order). A lone
    // acceptance at a limit price is either a resting limit order or a pending stop-limit:
    // the events alone cannot tell the two apart, and the request the caller sent says which.
    struct OrderAccepted {
        OrderId orderId;
        Side side;
        Price price;
        Quantity quantity;

        bool operator==(const OrderAccepted&) const = default;
    };

    // The submission was refused before anything changed.
    struct OrderRejected {
        OrderId orderId;
        RejectionReason reason;

        bool operator==(const OrderRejected&) const = default;
    };

    // One execution between the incoming order and one resting order, at the resting price.
    struct TradeExecuted {
        OrderId incomingOrderId;
        OrderId restingOrderId;
        Price executionPrice;
        Quantity executionQuantity;

        bool operator==(const TradeExecuted&) const = default;
    };

    // A resting order was removed; cancelledQuantity is what it still had left.
    struct OrderCancelled {
        OrderId orderId;
        Quantity cancelledQuantity;

        bool operator==(const OrderCancelled&) const = default;
    };

    // The cancellation was refused and the book left unchanged.
    struct CancelRejected {
        OrderId orderId;
        RejectionReason reason;

        bool operator==(const CancelRejected&) const = default;
    };

    // The accepted order's unfilled remainder did not rest, and left the book. Always the
    // last event of that order's part of the result.
    struct OrderExpired {
        OrderId orderId;
        Quantity expiredQuantity;

        bool operator==(const OrderExpired&) const = default;
    };

    // Self-trade prevention stopped a match between two orders with the same owner; never a
    // trade. price is the resting order's; cancelledQuantity is what the resting order had
    // left when it was removed, or 0 if it was left in place.
    struct SelfTradePrevented {
        OrderId incomingOrderId;
        OrderId restingOrderId;
        Price price;
        Quantity cancelledQuantity;

        bool operator==(const SelfTradePrevented&) const = default;
    };

    // A pending stop triggered on a trade at tradePrice, and runs now; the events that follow,
    // up to the next StopTriggered, are its own.
    struct StopTriggered {
        OrderId orderId;
        Price stopPrice;
        Price tradePrice;

        bool operator==(const StopTriggered&) const = default;
    };

    // A resting order was replaced under the same ID. quantity is the new total, including
    // what had executed; openQuantity is the new total less that, before any matching.
    // priorityKept says whether it kept its place in the queue.
    struct OrderReplaced {
        OrderId orderId;
        Side side;
        Price price;
        Quantity quantity;
        Quantity openQuantity;
        bool priorityKept;

        bool operator==(const OrderReplaced&) const = default;
    };

    // The replace was refused and the book left unchanged.
    struct ReplaceRejected {
        OrderId orderId;
        RejectionReason reason;

        bool operator==(const ReplaceRejected&) const = default;
    };

    // Open: later releases may append alternatives, never reorder or remove one, so code that
    // visits it should handle an event it does not know.
    using BookEvent = std::variant<
        OrderAccepted,
        OrderRejected,
        TradeExecuted,
        OrderCancelled,
        CancelRejected,
        OrderExpired,
        SelfTradePrevented,
        StopTriggered,
        OrderReplaced,
        ReplaceRejected
    >;

}  // namespace order_book
