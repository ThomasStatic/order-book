#include <stdexcept>

#include "order_book/order.hpp"

namespace order_book {

Order::Order(OrderId id, Price price, Quantity quantity, SequenceNum sequence, Side orderSide, OrderType orderType)
    : orderId(id),
      sequenceNum(sequence),
      priceTick(price),
      initialQuantity(quantity),
      remainingQuantity(quantity),
      side(orderSide),
      type(orderType),
      fillStatus(FillStatus::NONE) {
    if(sequence == 0) {
        throw std::invalid_argument("zero sequence number error");
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
    if(remainingQuantity == 0) {
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

OrderId Order::getOrderId() const {
    return orderId;
}

SequenceNum Order::getSequenceNum() const {
    return sequenceNum;
}

void Order::assignSequenceNum(SequenceNum sequence) {
    if(sequence == 0) {
        throw std::invalid_argument("zero sequence number error");
    }

    sequenceNum = sequence;
}

Price Order::getPriceTick() const {
    return priceTick;
}

Quantity Order::getInitialQuantity() const {
    return initialQuantity;
}

Quantity Order::getRemainingQuantity() const {
    return remainingQuantity;
}

Quantity Order::fillQuantity(Quantity amount) {
    if(amount == 0) {
        throw std::invalid_argument("zero fill quantity error");
    }

    if(fillStatus == FillStatus::FILLED) {
        throw std::runtime_error("filling an inactive order error");
    }

    if(amount >= remainingQuantity) {
        const Quantity excess = amount - remainingQuantity;
        remainingQuantity = 0;
        fillStatus = FillStatus::FILLED;
        return excess;
    }
    remainingQuantity -= amount;
    fillStatus = FillStatus::PARTIAL;
    return 0;
}

Side Order::getSide() const {
    return side;
}

OrderType Order::getOrderType() const {
    return type;
}

FillStatus Order::getFillStatus() const {
    return fillStatus;
}

void Order::setFillStatus(FillStatus status) {
    fillStatus = status;
}

bool Order::isActive() const {
    return fillStatus != FillStatus::FILLED;
}

}  // namespace order_book
