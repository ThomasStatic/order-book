#include <stdexcept>

#include "order_book/trade.hpp"

namespace order_book {
    Trade::Trade(TradeId id, OrderId restingOrderId, OrderId incomingOrderId, Price price, Quantity quantity,
                 Side incomingOrderSide)
        : tradeId(id),
          restingId(restingOrderId),
          incomingId(incomingOrderId),
          executionPrice(price),
          executionQuantity(quantity),
          incomingSide(incomingOrderSide) {
        if (id == 0) {
            throw std::invalid_argument("zero trade id error");
        }

        if (restingOrderId == 0) {
            throw std::invalid_argument("zero resting order id error");
        }

        if (incomingOrderId == 0) {
            throw std::invalid_argument("zero incoming order id error");
        }

        if (price == 0) {
            throw std::invalid_argument("zero execution price error");
        }

        if (quantity == 0) {
            throw std::invalid_argument("zero execution quantity error");
        }
    }

    TradeId Trade::getTradeId() const {
        return tradeId;
    }

    OrderId Trade::getRestingId() const {
        return restingId;
    }

    OrderId Trade::getIncomingId() const {
        return incomingId;
    }

    Price Trade::getExecutionPrice() const {
        return executionPrice;
    }

    Quantity Trade::getExecutionQuantity() const {
        return executionQuantity;
    }

    Side Trade::getIncomingSide() const {
        return incomingSide;
    }

    OrderId Trade::getBuyOrderId() const {
        return incomingSide == Side::BUY ? incomingId : restingId;
    }

    OrderId Trade::getSellOrderId() const {
        return incomingSide == Side::SELL ? incomingId : restingId;
    }
}  // namespace order_book
