#pragma once

#include <optional>
#include <stdexcept>
#include "fill_status.hpp"
#include "order_type.hpp"
#include "rejection_reason.hpp"
#include "side.hpp"

namespace order_book {

    class Order {
    private:
        // OrderBook alone assigns the sequence number, once, when it accepts the order.
        friend class OrderBook;
        // Test-only access to internals; defined in tests, never in the library.
        friend struct OrderBookTestAccess;

        unsigned int orderId;
        unsigned int sequenceNum;

        unsigned int priceTick;
        unsigned int initialQuant;
        unsigned int remainingQuant;

        Side side;
        OrderType type;
        FillStatus execution;

        // Replaces the sequence number given at construction; throws
        // std::invalid_argument on zero, leaving the order unchanged.
        void assignSequenceNum(unsigned int seq);

        // The one home for field checks, shared by the constructor and the book: a zero
        // ID, price, or remaining quantity, a side other than BUY or SELL, or an
        // unsupported type, in that order; nothing if every field is well formed.
        std::optional<RejectionReason> malformedReason() const;

    public:
        // Throws std::invalid_argument on a zero sequence number, or on any field
        // malformedReason() refuses, with that reason's rejectionMessage.
        Order(unsigned int id, unsigned int tick, unsigned int quant, unsigned int arrival, Side s,
              OrderType t = OrderType::LIMIT);

        unsigned int getOrderId() const;
        unsigned int getSequenceNum() const;

        unsigned int getPriceTick() const;
        unsigned int getInitialQuantity() const;
        unsigned int getRemainingQuantity() const;
        unsigned int fillQuantity(unsigned int amt);

        Side getSide() const;
        OrderType getOrderType() const;
        FillStatus getExecutionLevel() const;
        void setExecutionLevel(FillStatus newLevel);
        bool orderIsActive() const;
    };

}  // namespace order_book