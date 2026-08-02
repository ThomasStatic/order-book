#pragma once

#include <stdexcept>
#include "side.hpp"

namespace order_book {

    class Trade {
    private:
        unsigned int tradeId;
        unsigned int restingId;
        unsigned int incomingId;

        unsigned int executionPrice; // in tick quantity
        unsigned int executionQuantity;

        Side incomingSide;

    public:
        Trade(unsigned int tId, unsigned int rId, unsigned int iId, unsigned int execTick, unsigned int execQuant, Side iSide);

        unsigned int getTradeId() const;
        unsigned int getRestingId() const;
        unsigned int getIncomingId() const;

        unsigned int getExecutionPrice() const;
        unsigned int getExecutionQuantity() const;

        Side getIncomingSide() const;
    };

}  // namespace order_book