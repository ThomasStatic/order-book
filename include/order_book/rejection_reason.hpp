#pragma once

namespace order_book {
    enum class RejectionReason {
        NONE,
        DUPLICATE_ORDER_ID,
        ORDER_NOT_FOUND,
        ORDER_NOT_CANCELLABLE
    };
}