#pragma once

#include "order_type.hpp"
#include "side.hpp"
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
        Price price;  // the limit price, in ticks
        Quantity quantity;
        OrderType type = OrderType::LIMIT;
    };

}  // namespace order_book
