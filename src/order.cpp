#include "order_book/order.hpp"

namespace order_book {

Order::Order(unsigned int id, unsigned int tick, unsigned int quant, unsigned int seq, Side s): sequenceNum(seq), side(s) {
    if (id == 0) {
        throw std::invalid_argument("Non-positive order ID error.");
    }

    if (tick == 0) {
        throw std::invalid_argument("Non-positive price tick error.");
    }

    if(quant == 0) {
        throw std::invalid_argument("Non-positive quantity error.");
    }

    if(seq == 0) {
        throw std::invalid_argument("Non-positive sequence number error.");
    }

    orderId = id;
    priceTick = tick;
    initialQuant = quant;
    remainingQuant = quant;
    execution = FillStatus::NONE;
}

unsigned int Order::getOrderId() const{
    return orderId;
}

unsigned int Order::getSequenceNum() const {
    return sequenceNum;
}

void Order::decrementSequence() {
    if (sequenceNum == 0) {
        throw std::runtime_error("Decreasing 0 sequence number error.");
    }

    sequenceNum--;
}

unsigned int Order::getPriceTick() const {
    return priceTick;
}

unsigned int Order::getInitialQuantity() const {
    return initialQuant;
}

unsigned int Order::getRemainingQuantity() const {
    return remainingQuant;
}

unsigned int Order::fillQuantity(unsigned int amt) {
    if(amt == 0) {
        throw std::invalid_argument("Fill amount 0 error.");
    }

    if(execution == FillStatus::FILLED) {
        throw std::runtime_error("Filling inactive order error.");
    }
    
    unsigned int remainingAmt = amt;
    if(amt >= remainingQuant) {
        remainingAmt = amt - remainingQuant;
        remainingQuant = 0;
        execution = FillStatus::FILLED;
    }
    else {
        remainingAmt = 0;
        remainingQuant = remainingQuant - amt;
        execution = FillStatus::PARTIAL;
    }
    return remainingAmt;
}

Side Order::getSide() const {
    return side;
}

FillStatus Order::getExecutionLevel() const {
    return execution;
}

void Order::setExecutionLevel(FillStatus newLevel) {
    execution = newLevel;
}

bool Order::orderIsActive() const{
    return execution == FillStatus::FILLED ? false : true;
}

}  // namespace order_book

