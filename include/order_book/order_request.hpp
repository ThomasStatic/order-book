#pragma once

#include "order_type.hpp"
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
        Price price;  // the limit price, in ticks; 0 for a MARKET order, which has none
        Quantity quantity;
        OrderType type = OrderType::LIMIT;
        // GTC rests what does not trade at once; IOC expires it (reported by OrderExpired); FOK
        // fills whole or makes no trades and expires whole.
        // A MARKET order must be IOC or FOK: it can never rest.
        TimeInForce timeInForce = TimeInForce::GTC;
    };

}  // namespace order_book
