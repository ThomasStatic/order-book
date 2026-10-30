#pragma once

#include "order_type.hpp"
#include "self_trade_prevention.hpp"
#include "side.hpp"
#include "time_in_force.hpp"
#include "types.hpp"

namespace order_book {

    // A submission and the instructions for how it may trade, taken by
    // OrderBook::submitOrder(OrderRequest). A plain aggregate: building one never throws and
    // checks nothing, and it carries no sequence number. submitOrder checks it, and reports a
    // malformed request as a REJECTED result. With every field after quantity left at its
    // default, it is exactly a v1 limit order: submitOrder(Order) gives the same result for an
    // Order with the same ID, side, price, and quantity.
    //
    // Unlike Order, the resting record, a request is never stored: the book builds an Order
    // only for what rests (docs/adr/0001-order-request-separate-from-resting-order.md).
    struct OrderRequest {
        OrderId id;
        Side side;
        Price price;  // the limit price, in ticks; 0 for a MARKET or STOP order, which has none
        Quantity quantity;
        OrderType type = OrderType::LIMIT;
        // GTC rests what does not trade at once; IOC expires it (reported by OrderExpired); FOK
        // fills whole or makes no trades and expires whole.
        // A MARKET or STOP order must be IOC or FOK: it can never rest.
        TimeInForce timeInForce = TimeInForce::GTC;
        // FIX StopPx: the price a trade on this book must reach (at or above for a buy, at or
        // below for a sell) to trigger a STOP or STOP_LIMIT order. 0, none, on any other type.
        Price stopPrice = 0;
        // Post-only (FIX ExecInst=6, participate don't initiate), on a GTC LIMIT order only: it
        // rests, or if it would take liquidity on arrival (at or through the best opposing
        // price) it is accepted and wholly expired, never repriced. The flag is not kept once it
        // rests.
        bool postOnly = false;
        // Self-trade prevention: owner 0 is none. With an owner, a match with a resting order of
        // the same owner is prevented as selfTradePrevention says, never traded. The mode must be
        // valid whatever the owner; with owner 0 it has no effect.
        OwnerId owner = 0;
        SelfTradePrevention selfTradePrevention = SelfTradePrevention::CANCEL_RESTING;

        bool operator==(const OrderRequest&) const = default;
    };

}  // namespace order_book
