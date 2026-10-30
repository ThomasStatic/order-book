#pragma once

namespace order_book {
    // Open: later releases may append reasons, never renumber or remove one, so code that
    // switches over it should keep a default case.
    enum class RejectionReason {
        NONE,
        DUPLICATE_ORDER_ID,
        ORDER_NOT_FOUND,
        // Malformed submissions, refused before matching.
        INVALID_ORDER_ID,       // the reserved ID 0
        INVALID_PRICE,          // zero price
        INVALID_QUANTITY,       // no remaining quantity
        INVALID_SIDE,           // neither BUY nor SELL
        UNSUPPORTED_ORDER_TYPE,
        ORDER_NOT_ACTIVE,       // marked FILLED
        WOULD_CROSS_BOOK,       // addOrder only: it rests without matching, so it refuses to cross
        // INVALID_X is a value unusable on its own; X_NOT_ALLOWED a usable value in a
        // forbidden combination.
        PRICE_NOT_ALLOWED,              // a price on a MARKET or STOP order
        INVALID_TIME_IN_FORCE,          // neither GTC, IOC, nor FOK
        TIME_IN_FORCE_NOT_ALLOWED,      // a MARKET or STOP order with GTC
        INVALID_STOP_PRICE,             // no stop price on a STOP or STOP_LIMIT order
        STOP_PRICE_NOT_ALLOWED,         // a stop price on a LIMIT or MARKET order
        POST_ONLY_NOT_ALLOWED,          // post-only on anything but a GTC LIMIT order
        INVALID_SELF_TRADE_PREVENTION,  // not a known mode
        // Replace only.
        REPLACE_QUANTITY_TOO_LOW,       // the new total is at or below what has executed
        ORDER_NOT_REPLACEABLE           // the ID is a pending stop
    };

    // The enumerator's name, for printing reasons as identifiers.
    constexpr const char* rejectionReasonName(RejectionReason reason) {
        switch(reason) {
            case RejectionReason::NONE: return "NONE";
            case RejectionReason::DUPLICATE_ORDER_ID: return "DUPLICATE_ORDER_ID";
            case RejectionReason::ORDER_NOT_FOUND: return "ORDER_NOT_FOUND";
            case RejectionReason::INVALID_ORDER_ID: return "INVALID_ORDER_ID";
            case RejectionReason::INVALID_PRICE: return "INVALID_PRICE";
            case RejectionReason::INVALID_QUANTITY: return "INVALID_QUANTITY";
            case RejectionReason::INVALID_SIDE: return "INVALID_SIDE";
            case RejectionReason::UNSUPPORTED_ORDER_TYPE: return "UNSUPPORTED_ORDER_TYPE";
            case RejectionReason::ORDER_NOT_ACTIVE: return "ORDER_NOT_ACTIVE";
            case RejectionReason::WOULD_CROSS_BOOK: return "WOULD_CROSS_BOOK";
            case RejectionReason::PRICE_NOT_ALLOWED: return "PRICE_NOT_ALLOWED";
            case RejectionReason::INVALID_TIME_IN_FORCE: return "INVALID_TIME_IN_FORCE";
            case RejectionReason::TIME_IN_FORCE_NOT_ALLOWED: return "TIME_IN_FORCE_NOT_ALLOWED";
            case RejectionReason::INVALID_STOP_PRICE: return "INVALID_STOP_PRICE";
            case RejectionReason::STOP_PRICE_NOT_ALLOWED: return "STOP_PRICE_NOT_ALLOWED";
            case RejectionReason::POST_ONLY_NOT_ALLOWED: return "POST_ONLY_NOT_ALLOWED";
            case RejectionReason::INVALID_SELF_TRADE_PREVENTION: return "INVALID_SELF_TRADE_PREVENTION";
            case RejectionReason::REPLACE_QUANTITY_TOO_LOW: return "REPLACE_QUANTITY_TOO_LOW";
            case RejectionReason::ORDER_NOT_REPLACEABLE: return "ORDER_NOT_REPLACEABLE";
        }
        return "UNKNOWN";
    }

    // A distinct message per reason, for callers that report a rejection by throwing.
    constexpr const char* rejectionMessage(RejectionReason reason) {
        switch(reason) {
            case RejectionReason::NONE: return "no rejection";
            case RejectionReason::DUPLICATE_ORDER_ID: return "duplicate active order id error";
            case RejectionReason::ORDER_NOT_FOUND: return "order not found error";
            case RejectionReason::INVALID_ORDER_ID: return "reserved order id 0 error";
            case RejectionReason::INVALID_PRICE: return "non-positive price tick error";
            case RejectionReason::INVALID_QUANTITY: return "no remaining quantity error";
            case RejectionReason::INVALID_SIDE: return "order side neither BUY nor SELL error";
            case RejectionReason::UNSUPPORTED_ORDER_TYPE: return "unsupported order type error";
            case RejectionReason::ORDER_NOT_ACTIVE: return "order marked FILLED error";
            case RejectionReason::WOULD_CROSS_BOOK: return "added order would cross the book error";
            case RejectionReason::PRICE_NOT_ALLOWED: return "price given on a market or stop order error";
            case RejectionReason::INVALID_TIME_IN_FORCE: return "time in force neither GTC, IOC, nor FOK error";
            case RejectionReason::TIME_IN_FORCE_NOT_ALLOWED: return "good-till-cancelled market or stop order error";
            case RejectionReason::INVALID_STOP_PRICE: return "missing stop price on a stop order error";
            case RejectionReason::STOP_PRICE_NOT_ALLOWED: return "stop price given on an order that is not a stop error";
            case RejectionReason::POST_ONLY_NOT_ALLOWED: return "post-only on an order that is not a GTC limit order error";
            case RejectionReason::INVALID_SELF_TRADE_PREVENTION: return "self-trade prevention mode not a known mode error";
            case RejectionReason::REPLACE_QUANTITY_TOO_LOW: return "replace total at or below executed quantity error";
            case RejectionReason::ORDER_NOT_REPLACEABLE: return "pending stop order cannot be replaced error";
        }
        return "unknown rejection reason error";
    }
}