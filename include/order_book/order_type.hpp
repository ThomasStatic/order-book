#pragma once

namespace order_book {
    enum class OrderType {
        LIMIT
    };

    // Whether the value is one of the enumerators, which an OrderRequest may carry; false
    // for one made by a bad cast.
    constexpr bool isValidOrderType(OrderType type) {
        return type == OrderType::LIMIT;
    }

    // Whether an order of this type can rest, which is what an Order accepts: LIMIT only.
    constexpr bool isSupportedOrderType(OrderType type) {
        return type == OrderType::LIMIT;
    }
}  // namespace order_book
