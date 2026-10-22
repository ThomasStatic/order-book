#pragma once

namespace order_book {
    enum class OrderType {
        LIMIT
    };

    // False for any value outside the enumerators, e.g. one made by a bad cast.
    constexpr bool isSupportedOrderType(OrderType type) {
        return type == OrderType::LIMIT;
    }
}  // namespace order_book
