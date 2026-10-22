#include <cstdlib>
#include <iostream>
#include <string>

#include "order_book/book_event.hpp"
#include "order_book/event_format.hpp"

namespace {

using order_book::CancelRejected;
using order_book::formatEvent;
using order_book::OrderAccepted;
using order_book::OrderCancelled;
using order_book::OrderRejected;
using order_book::RejectionReason;
using order_book::Side;
using order_book::TradeExecuted;

void expectFormat(const order_book::BookEvent& event, const std::string& expected) {
    const std::string actual = formatEvent(event);
    if (actual != expected) {
        std::cerr << "FAILED: expected \"" << expected << "\", got \"" << actual << "\"" << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

}  // namespace

int main() {
    expectFormat(OrderAccepted{4, Side::BUY, 101, 6}, "OrderAccepted id=4 side=BUY price=101 qty=6");
    expectFormat(OrderAccepted{5, Side::SELL, 98, 10}, "OrderAccepted id=5 side=SELL price=98 qty=10");
    expectFormat(OrderRejected{2, RejectionReason::DUPLICATE_ORDER_ID}, "OrderRejected id=2 reason=DUPLICATE_ORDER_ID");
    expectFormat(OrderRejected{0, RejectionReason::INVALID_ORDER_ID}, "OrderRejected id=0 reason=INVALID_ORDER_ID");
    expectFormat(TradeExecuted{4, 2, 100, 3}, "TradeExecuted incoming=4 resting=2 price=100 qty=3");
    expectFormat(OrderCancelled{1, 30}, "OrderCancelled id=1 qty=30");
    expectFormat(CancelRejected{2, RejectionReason::ORDER_NOT_FOUND}, "CancelRejected id=2 reason=ORDER_NOT_FOUND");

    std::cout << "event format tests passed" << std::endl;
    return EXIT_SUCCESS;
}
