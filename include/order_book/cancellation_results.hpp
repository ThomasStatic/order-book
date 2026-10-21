#pragma once

#include <optional>
#include <stdexcept>

#include "rejection_reason.hpp"

namespace order_book {
    // Returned by OrderBook::cancelOrder: success with no reason, or failure with one.
    struct CancellationResult {
        bool success;
        std::optional<RejectionReason> rejectionReason;
    };

    enum class CancellationStatus {
        CANCELLED,
        REJECTED
    };

    class CancellationResults {
    private:
        unsigned int targetedOrder;
        unsigned int remainingQuantity;

        RejectionReason rejectionReason;
        CancellationStatus status;

    public:
        CancellationResults(unsigned int tOrder);
    
        unsigned int getTargetedOrder() const;
        unsigned int getRemainingQuantity() const;
        
        RejectionReason getRejectionReason() const;
        CancellationStatus getStatus() const;

    };
}