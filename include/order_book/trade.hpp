#pragma once

#include "side.hpp"
#include "types.hpp"

namespace order_book {

    class Trade {
    private:
        TradeId tradeId;
        OrderId restingId;
        OrderId incomingId;

        Price executionPrice;  // the resting order's price
        Quantity executionQuantity;

        Side incomingSide;

    public:
        // Throws std::invalid_argument if any ID, the price, or the quantity is zero.
        Trade(TradeId id, OrderId restingOrderId, OrderId incomingOrderId, Price price, Quantity quantity,
              Side incomingOrderSide);

        TradeId getTradeId() const;
        OrderId getRestingId() const;
        OrderId getIncomingId() const;

        Price getExecutionPrice() const;
        Quantity getExecutionQuantity() const;

        Side getIncomingSide() const;

        // The buy and sell orders, whichever was resting.
        OrderId getBuyOrderId() const;
        OrderId getSellOrderId() const;
    };

}  // namespace order_book