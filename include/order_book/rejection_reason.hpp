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

    // A distinct message per reason, for callers that report a rejection by throwing.
    constexpr const char* rejectionMessage(RejectionReason reason) {
        switch(reason) {
            case RejectionReason::NONE: return "no rejection";
            case RejectionReason::DUPLICATE_ORDER_ID: return "duplicate active order id error";
            case RejectionReason::ORDER_NOT_FOUND: return "order not found error";
            case RejectionReason::ORDER_NOT_CANCELLABLE: return "order not cancellable error";
            case RejectionReason::INVALID_ORDER_ID: return "reserved order id 0 error";
            case RejectionReason::INVALID_PRICE: return "non-positive price tick error";
            case RejectionReason::INVALID_QUANTITY: return "no remaining quantity error";
            case RejectionReason::INVALID_SIDE: return "order side neither BUY nor SELL error";
            case RejectionReason::UNSUPPORTED_ORDER_TYPE: return "unsupported order type error";
            case RejectionReason::ORDER_NOT_ACTIVE: return "order marked FILLED error";
        }
        return "unknown rejection reason error";
    }
}