#pragma once

#include <stdexcept>

#include "rejection_reason.hpp"

namespace order_book {
    enum class CancellationStatus {
        CANCELLED,
        REJECTED
    };

    class CancellationResult {
    private:
        unsigned int targetedOrder;
        unsigned int remainingQuantity;

        RejectionReason rejectionReason;
        CancellationStatus status;

    public:
        CancellationResult(unsigned int tOrder);
    
        unsigned int getTargetedOrder() const;
        unsigned int getRemainingQuantity() const;
        
        RejectionReason getRejectionReason() const;
        CancellationStatus getStatus() const;

    };
}