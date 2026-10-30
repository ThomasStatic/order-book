#include <stdexcept>
#include <utility>

#include "order_book/replace_result.hpp"

namespace order_book {
    ReplaceResult::ReplaceResult(bool succeeded, std::optional<RejectionReason> reason, SubmissionTrades trades,
                                 SubmissionEvents recordedEvents, Quantity remaining, FillStatus fill)
        : success(succeeded),
          rejectionReason(reason),
          generatedTrades(std::move(trades)),
          events(std::move(recordedEvents)),
          remainingQuantity(remaining),
          fillStatus(fill) {}

    ReplaceResult ReplaceResult::rejected(OrderId orderId, RejectionReason reason) {
        if (reason == RejectionReason::NONE) {
            throw std::invalid_argument("rejected replace without a rejection reason error");
        }
        return ReplaceResult(false, reason, {}, SubmissionEvents{ReplaceRejected{orderId, reason}}, 0, FillStatus::NONE);
    }

    bool ReplaceResult::isSuccess() const {
        return success;
    }

    std::optional<RejectionReason> ReplaceResult::getRejectionReason() const {
        return rejectionReason;
    }

    const SubmissionEvents& ReplaceResult::getEvents() const {
        return events;
    }

    const SubmissionTrades& ReplaceResult::getGeneratedTrades() const {
        return generatedTrades;
    }

    Quantity ReplaceResult::getRemainingQuantity() const {
        return remainingQuantity;
    }

    FillStatus ReplaceResult::getFillStatus() const {
        return fillStatus;
    }
}  // namespace order_book
