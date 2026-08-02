#include "order_book/cancellation_results.hpp"

namespace order_book {
    CancellationResult::CancellationResult(unsigned int tOrder) {
        if (tOrder == 0) {
            throw std::invalid_argument("Non-positive target order ID error.");
        }

        targetedOrder = tOrder;
        remainingQuantity = 0;
        rejectionReason = RejectionReason::NONE;
        status = CancellationStatus::CANCELLED;
    }
    
    unsigned int CancellationResult::getTargetedOrder() const {
        return targetedOrder;
    }

    unsigned int CancellationResult::getRemainingQuantity() const {
        return remainingQuantity;
    }
    
    RejectionReason CancellationResult::getRejectionReason() const {
        return rejectionReason;
    }

    CancellationStatus CancellationResult::getStatus() const {
        return status;
    }

}