#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

#include "order_book/book_event.hpp"
#include "order_book/event_format.hpp"

namespace {

using order_book::CancelRejected;
using order_book::formatEvent;
using order_book::OrderAccepted;
using order_book::OrderCancelled;
using order_book::OrderExpired;
using order_book::OrderRejected;
using order_book::OrderReplaced;
using order_book::ReplaceRejected;
using order_book::SelfTradePrevented;
using order_book::StopTriggered;
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

    // An acceptance at price 0 is a market or STOP order; no other event prints MARKET.
    expectFormat(OrderAccepted{3, Side::BUY, 0, 10}, "OrderAccepted id=3 side=BUY price=MARKET qty=10");
    expectFormat(OrderAccepted{3, Side::SELL, 0, 10}, "OrderAccepted id=3 side=SELL price=MARKET qty=10");
    expectFormat(OrderAccepted{3, Side::BUY, 1, 10}, "OrderAccepted id=3 side=BUY price=1 qty=10");
    expectFormat(OrderExpired{3, 4}, "OrderExpired id=3 qty=4");
    expectFormat(SelfTradePrevented{3, 1, 100, 5}, "SelfTradePrevented incoming=3 resting=1 price=100 qty=5");
    expectFormat(SelfTradePrevented{3, 1, 100, 0}, "SelfTradePrevented incoming=3 resting=1 price=100 qty=0");
    expectFormat(StopTriggered{9, 100, 98}, "StopTriggered id=9 stop_price=100 trade_price=98");
    expectFormat(OrderReplaced{5, Side::BUY, 101, 20, 12, true},
                 "OrderReplaced id=5 side=BUY price=101 qty=20 open=12 priority=KEPT");
    expectFormat(OrderReplaced{5, Side::BUY, 102, 25, 17, false},
                 "OrderReplaced id=5 side=BUY price=102 qty=25 open=17 priority=LOST");
    expectFormat(OrderReplaced{6, Side::SELL, 99, 4294967295u, 4294967295u, false},
                 "OrderReplaced id=6 side=SELL price=99 qty=4294967295 open=4294967295 priority=LOST");
    expectFormat(ReplaceRejected{5, RejectionReason::REPLACE_QUANTITY_TOO_LOW},
                 "ReplaceRejected id=5 reason=REPLACE_QUANTITY_TOO_LOW");

    // Every new reason prints as its own name.
    const std::pair<RejectionReason, const char*> reasons[] = {
        {RejectionReason::PRICE_NOT_ALLOWED, "PRICE_NOT_ALLOWED"},
        {RejectionReason::INVALID_TIME_IN_FORCE, "INVALID_TIME_IN_FORCE"},
        {RejectionReason::TIME_IN_FORCE_NOT_ALLOWED, "TIME_IN_FORCE_NOT_ALLOWED"},
        {RejectionReason::INVALID_STOP_PRICE, "INVALID_STOP_PRICE"},
        {RejectionReason::STOP_PRICE_NOT_ALLOWED, "STOP_PRICE_NOT_ALLOWED"},
        {RejectionReason::POST_ONLY_NOT_ALLOWED, "POST_ONLY_NOT_ALLOWED"},
        {RejectionReason::INVALID_SELF_TRADE_PREVENTION, "INVALID_SELF_TRADE_PREVENTION"},
        {RejectionReason::REPLACE_QUANTITY_TOO_LOW, "REPLACE_QUANTITY_TOO_LOW"},
        {RejectionReason::ORDER_NOT_REPLACEABLE, "ORDER_NOT_REPLACEABLE"},
    };
    for (const auto& [reason, name] : reasons) {
        expectFormat(OrderRejected{7, reason}, std::string("OrderRejected id=7 reason=") + name);
    }

    std::cout << "event format tests passed" << std::endl;
    return EXIT_SUCCESS;
}
