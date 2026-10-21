#pragma once

#include <optional>

#include "rejection_reason.hpp"

namespace order_book {
    // Returned by OrderBook::cancelOrder: success with no reason, or failure with one.
    struct CancellationResult {
        bool success;
        std::optional<RejectionReason> rejectionReason;
    };
}