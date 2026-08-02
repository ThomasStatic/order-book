#include "order_book/trade.hpp"

namespace order_book {
    Trade::Trade(unsigned int tId, unsigned int rId, unsigned int iId, unsigned int execTick, unsigned int execQuant, Side iSide): incomingSide(iSide) {
        if (tId == 0) {
        throw std::invalid_argument("Non-positive trade ID error.");
        }

        if (rId == 0) {
        throw std::invalid_argument("Non-positive resting ID error.");
        }

        if (iId == 0) {
        throw std::invalid_argument("Non-positive incoming ID error.");
        }

        if (execTick == 0) {
        throw std::invalid_argument("Non-positive execution price error.");
        }

        if (execQuant == 0) {
        throw std::invalid_argument("Non-positive execution quantity error.");
        }

        tradeId = tId;
        restingId = rId;
        incomingId = iId;
        executionPrice = execTick;
        executionQuantity = execQuant;
    }

    unsigned int Trade::getTradeId() const {
        return tradeId;
    }

    unsigned int Trade::getRestingId() const {
        return restingId;
    }

    unsigned int Trade::getIncomingId() const {
        return incomingId;
    }

    unsigned int Trade::getExecutionPrice() const {
        return executionPrice;
    }

    unsigned int Trade::getExecutionQuantity() const {
        return executionQuantity;
    }

    Side Trade::getIncomingSide() const {
        return incomingSide;
    }
}