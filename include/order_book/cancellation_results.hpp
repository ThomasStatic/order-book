#pragma once

#include <optional>
#include <vector>

#include "book_event.hpp"
#include "rejection_reason.hpp"

namespace order_book {
    // Returned by OrderBook::cancelOrder: success with no reason, or failure with one.
    // events holds exactly one OrderCancelled on success, or one CancelRejected on failure.
    struct CancellationResult {
        bool success;
        std::optional<RejectionReason> rejectionReason;
        std::vector<BookEvent> events;
    };
}
