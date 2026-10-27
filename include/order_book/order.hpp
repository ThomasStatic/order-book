#pragma once

#include <optional>

#include "fill_status.hpp"
#include "order_type.hpp"
#include "rejection_reason.hpp"
#include "side.hpp"
#include "types.hpp"

namespace order_book {

    class Order {
    private:
        // OrderBook alone assigns the sequence number, once, when it accepts the order.
        friend class OrderBook;
        // Test-only access to internals; defined in tests, never in the library.
        friend struct OrderBookTestAccess;

        OrderId orderId;
        SequenceNum sequenceNum;

        Price priceTick;
        Quantity initialQuantity;
        Quantity remainingQuantity;

        Side side;
        OrderType type;
        FillStatus fillStatus;

        // Replaces the sequence number given at construction; throws
        // std::invalid_argument on zero, leaving the order unchanged.
        void assignSequenceNum(SequenceNum sequence);

        // The one home for field checks, shared by the constructor and the book: a zero
        // ID, price, or remaining quantity, a side other than BUY or SELL, or an
        // unsupported type, in that order; nothing if every field is well formed.
        std::optional<RejectionReason> malformedReason() const;

    public:
        // Throws std::invalid_argument on a zero sequence number, or on any field
        // malformedReason() refuses, with that reason's rejectionMessage. A book replaces
        // the sequence number with its own when it accepts the order.
        Order(OrderId id, Price price, Quantity quantity, SequenceNum sequence, Side orderSide,
              OrderType orderType = OrderType::LIMIT);

        OrderId getOrderId() const;
        SequenceNum getSequenceNum() const;

        Price getPriceTick() const;
        Quantity getInitialQuantity() const;
        Quantity getRemainingQuantity() const;
        // Fills up to amount and returns the part of it this order could not take. Throws
        // std::invalid_argument on zero, or std::runtime_error if the order is FILLED.
        Quantity fillQuantity(Quantity amount);

        Side getSide() const;
        OrderType getOrderType() const;
        FillStatus getFillStatus() const;
        void setFillStatus(FillStatus status);
        // Whether the order can still trade: anything but FILLED.
        bool isActive() const;
    };

}  // namespace order_book