#pragma once

namespace order_book {
    enum class Side {
            BUY,
            SELL
        };

    // False for any value outside the enumerators, e.g. one made by a bad cast.
    constexpr bool isValidSide(Side side) {
        return side == Side::BUY || side == Side::SELL;
    }
}  // namespace order_book
