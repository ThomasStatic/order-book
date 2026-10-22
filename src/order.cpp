#include "order_book/order.hpp"

namespace order_book {

Order::Order(unsigned int id, unsigned int tick, unsigned int quant, unsigned int seq, Side s, OrderType t)
    : orderId(id),
      sequenceNum(seq),
      priceTick(tick),
      initialQuant(quant),
      remainingQuant(quant),
      side(s),
      type(t),
      execution(FillStatus::NONE) {
    if(seq == 0) {
        throw std::invalid_argument("Non-positive sequence number error.");
    }

    if(const std::optional<RejectionReason> reason = malformedReason()) {
        throw std::invalid_argument(rejectionMessage(*reason));
    }
}

std::optional<RejectionReason> Order::malformedReason() const {
    if(orderId == 0) {
        return RejectionReason::INVALID_ORDER_ID;
    }
    if(priceTick == 0) {
        return RejectionReason::INVALID_PRICE;
    }
    if(remainingQuant == 0) {
        return RejectionReason::INVALID_QUANTITY;
    }
    if(!isValidSide(side)) {
        return RejectionReason::INVALID_SIDE;
    }
    if(!isSupportedOrderType(type)) {
        return RejectionReason::UNSUPPORTED_ORDER_TYPE;
    }
    return std::nullopt;
}

unsigned int Order::getOrderId() const{
    return orderId;
}

unsigned int Order::getSequenceNum() const {
    return sequenceNum;
}

void Order::assignSequenceNum(unsigned int seq) {
    if (seq == 0) {
        throw std::invalid_argument("Non-positive sequence number error.");
    }

    sequenceNum = seq;
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

OrderType Order::getOrderType() const {
    return type;
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

