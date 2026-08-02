#include "order_book/cancellation_results.hpp"

namespace order_book {
    CancellationResults::CancellationResults(unsigned int tOrder) {
        if (tOrder == 0) {
            throw std::invalid_argument("Non-positive target order ID error.");
        }

        targetedOrder = tOrder;
        remainingQuantity = 0;
        rejectionReason = RejectionReason::NONE;
        status = CancellationStatus::CANCELLED;
    }
    
    unsigned int CancellationResults::getTargetedOrder() const {
        return targetedOrder;
    }

    unsigned int CancellationResults::getRemainingQuantity() const {
        return remainingQuantity;
    }
    
    RejectionReason CancellationResults::getRejectionReason() const {
        return rejectionReason;
    }

    CancellationStatus CancellationResults::getStatus() const {
        return status;
    }

}