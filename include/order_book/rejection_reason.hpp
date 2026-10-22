#pragma once

namespace order_book {
    enum class RejectionReason {
        NONE,
        DUPLICATE_ORDER_ID,
        ORDER_NOT_FOUND,
        ORDER_NOT_CANCELLABLE,
        // Malformed submissions, refused before matching.
        INVALID_ORDER_ID,       // the reserved ID 0
        INVALID_PRICE,          // zero price
        INVALID_QUANTITY,       // no remaining quantity
        INVALID_SIDE,           // neither BUY nor SELL
        UNSUPPORTED_ORDER_TYPE,
        ORDER_NOT_ACTIVE        // marked FILLED
    };
}