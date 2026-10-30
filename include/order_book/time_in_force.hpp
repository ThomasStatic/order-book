#pragma once

namespace order_book {
    // How long an order may wait to trade (FIX TimeInForce, with this book's own values, not
    // FIX's wire values). GTC rests whatever does not trade at once; IOC trades what it can on
    // arrival and expires the rest. Only an OrderRequest carries one: an Order that rests is GTC.
    enum class TimeInForce {
        GTC = 0,
        IOC = 1
    };

    // False for any value outside the enumerators, e.g. one made by a bad cast.
    constexpr bool isValidTimeInForce(TimeInForce timeInForce) {
        return timeInForce == TimeInForce::GTC || timeInForce == TimeInForce::IOC;
    }
}  // namespace order_book
