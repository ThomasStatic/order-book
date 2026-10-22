#pragma once

#include <variant>

#include "rejection_reason.hpp"
#include "side.hpp"
#include "types.hpp"

namespace order_book {

    // Lifecycle events record what a command did to the book. They are domain output, held
    // by the command's result in the order they happened, and never printed by the engine.

    // The order passed validation and is about to be matched; quantity is what it had left
    // to trade on acceptance, before any matching (its whole size, for a fresh order).
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

    using BookEvent = std::variant<
        OrderAccepted,
        OrderRejected,
        TradeExecuted,
        OrderCancelled,
        CancelRejected
    >;

}  // namespace order_book
