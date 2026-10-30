#include <stdexcept>
#include <utility>

#include "order_book/submission_result.hpp"

namespace order_book {
    SubmissionResult::SubmissionResult(OrderId sId, SubmissionTrades&& trades, SubmissionEvents&& recordedEvents,
                                       Quantity remaining, FillStatus fill)
        : submissionId(sId),
          generatedTrades(std::move(trades)),
          remainingQuantity(remaining),
          fillStatus(fill),
          status(SubmissionStatus::ACCEPTED),
          rejectionReason(RejectionReason::NONE),
          events(std::move(recordedEvents)) {}

    SubmissionResult SubmissionResult::accepted(const OrderAccepted& acceptance, SubmissionTrades trades,
                                                Quantity remaining) {
        // The v1-shaped case: a limit order, so a limit price is required.
        if (acceptance.price == 0) {
            throw std::invalid_argument("accepted result with a zero price error");
        }
        ResultRecorder recorder(acceptance);
        recorder.adoptTrades(std::move(trades));
        if (remaining != recorder.current.open) {
            throw std::invalid_argument("accepted result whose trades and remainder do not sum to its quantity error");
        }
        return recorder.finish();
    }

    SubmissionResult::SubmissionResult(OrderId sId, Quantity remaining, RejectionReason reason)
        : submissionId(sId),
          remainingQuantity(remaining),
          fillStatus(FillStatus::NONE),
          status(SubmissionStatus::REJECTED),
          rejectionReason(reason),
          events{OrderRejected{sId, reason}} {}

    SubmissionResult SubmissionResult::rejected(OrderId orderId, Quantity remaining, RejectionReason reason) {
        if (reason == RejectionReason::NONE) {
            throw std::invalid_argument("rejected submission without a rejection reason error");
        }
        return SubmissionResult(orderId, remaining, reason);
    }

    OrderId SubmissionResult::getSubmissionId() const {
        return submissionId;
    }

    const SubmissionTrades& SubmissionResult::getGeneratedTrades() const {
        return generatedTrades;
    }

    Quantity SubmissionResult::getRemainingQuantity() const {
        return remainingQuantity;
    }

    FillStatus SubmissionResult::getFillStatus() const {
        return fillStatus;
    }

    SubmissionStatus SubmissionResult::getStatus() const {
        return status;
    }

    RejectionReason SubmissionResult::getRejectionReason() const {
        return rejectionReason;
    }

    const SubmissionEvents& SubmissionResult::getEvents() const {
        return events;
    }
}