#pragma once

namespace order_book {
    // FIX OrdType, with this book's own values, not FIX's wire values. A LIMIT order trades at
    // its limit price or better; a MARKET order has no price and trades at whatever the book
    // offers. Only a LIMIT order can rest, so only it can be an Order.
    enum class OrderType {
        LIMIT = 0,
        MARKET = 1
    };

    // Whether the value is one of the enumerators, which an OrderRequest may carry; false
    // for one made by a bad cast.
    constexpr bool isValidOrderType(OrderType type) {
        return type == OrderType::LIMIT || type == OrderType::MARKET;
    }

    // Whether an order of this type carries no price (it must be 0 on the request).
    constexpr bool hasNoPrice(OrderType type) {
        return type == OrderType::MARKET;
    }

    // Whether an order of this type can rest, which is what an Order accepts: LIMIT only.
    constexpr bool isSupportedOrderType(OrderType type) {
        return type == OrderType::LIMIT;
    }
}  // namespace order_book
