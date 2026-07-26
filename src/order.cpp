#include "order_book/order.hpp"

using namespace order; 

Order::Order(unsigned int id, unsigned int tick, unsigned int quant, unsigned int arrival, Side s): side(s) {
    if (id == 0) {
        throw std::invalid_argument("Non-positive order ID error.");
    }

    if (tick == 0) {
        throw std::invalid_argument("Non-positive price tick error.");
    }

    if(quant == 0) {
        throw std::invalid_argument("Non-positive quantity error.");
    }

    if(arrival == 0) {
        throw std::invalid_argument("Non-positive arrival number error.");;
    }

    orderId = id;
    priceTick = tick;
    quantity = quant;
    orderArrival = arrival;
    execution = ExecutionLifeCycle::NONE;
}

unsigned int Order::getOrderId() const{
    return orderId;
}

unsigned int Order::getOrderArrival() const {
    return orderArrival;
}

unsigned int Order::getPriceTick() const {
    return priceTick;
}

unsigned int Order::getQuantity() const {
    return quantity;
}

Side Order::getSide() const {
    return side;
}

ExecutionLifeCycle Order::getExecutionLevel() const {
    return execution;
}

void Order::setExecutionLevel(ExecutionLifeCycle newLevel) {
    execution = newLevel;
}

