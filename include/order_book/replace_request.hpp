#pragma once

#include "self_trade_prevention.hpp"
#include "types.hpp"

namespace order_book {

    // A replace of a resting order, taken by OrderBook::replaceOrder. The order keeps its ID
    // (unlike FIX's ClOrdID chaining: a synchronous call needs no second ID), and quantity is
    // the new total, including what has already traded
    // (docs/adr/0004-replace-keeps-the-order-id-and-takes-the-total-quantity.md). Side, type,
    // time in force, and stop price cannot change. Every instruction is declared again: one
    // left out takes its default, so omitting the owner clears it. A plain aggregate, checked
    // by replaceOrder.
    struct ReplaceRequest {
        OrderId id;
        Price price;
        Quantity quantity;  // the new total
        bool postOnly = false;
        OwnerId owner = 0;
        SelfTradePrevention selfTradePrevention = SelfTradePrevention::CANCEL_RESTING;

        bool operator==(const ReplaceRequest&) const = default;
    };

}  // namespace order_book
