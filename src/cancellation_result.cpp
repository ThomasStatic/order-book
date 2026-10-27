#include <stdexcept>
#include <utility>

#include "order_book/cancellation_result.hpp"

namespace order_book {
    CancellationResult::CancellationResult(bool succeeded, std::optional<RejectionReason> reason, BookEvent event)
        : success(succeeded),
          rejectionReason(reason),
          events{std::move(event)} {}

    CancellationResult CancellationResult::cancelled(OrderId orderId, Quantity cancelledQuantity) {
        if (orderId == 0) {
            throw std::invalid_argument("cancelled result for the reserved order id 0 error");
        }
        if (cancelledQuantity == 0) {
            throw std::invalid_argument("cancelled result with no cancelled quantity error");
        }
        return CancellationResult(true, std::nullopt, OrderCancelled{orderId, cancelledQuantity});
    }

    CancellationResult CancellationResult::rejected(OrderId orderId, RejectionReason reason) {
        if (reason == RejectionReason::NONE) {
            throw std::invalid_argument("rejected cancellation without a rejection reason error");
        }
        return CancellationResult(false, reason, CancelRejected{orderId, reason});
    }

    bool CancellationResult::isSuccess() const {
        return success;
    }

    std::optional<RejectionReason> CancellationResult::getRejectionReason() const {
        return rejectionReason;
    }

    const CancellationEvents& CancellationResult::getEvents() const {
        return events;
    }
}
