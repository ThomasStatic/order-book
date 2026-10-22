#include <utility>

#include "order_book/submission_results.hpp"

namespace order_book {
    SubmissionResults::SubmissionResults(unsigned int sId) {
        if (sId == 0) {
            throw std::invalid_argument("Non-positive submission ID error.");
        }

        submissionId = sId;
        generatedTrades = {};
        remainingQuantity = 0;
        fillStatus = FillStatus::NONE;
        status = SubmissionStatus::ACCEPTED;
        rejectionReason = RejectionReason::NONE; 
    }

    SubmissionResults SubmissionResults::accepted(unsigned int orderId, std::vector<Trade> trades,
                                                  unsigned int remaining) {
        SubmissionResults result(orderId);
        result.fillStatus = trades.empty() ? FillStatus::NONE
                          : remaining == 0 ? FillStatus::FILLED
                                           : FillStatus::PARTIAL;
        result.generatedTrades = std::move(trades);
        result.remainingQuantity = remaining;
        return result;
    }

    SubmissionResults::SubmissionResults(unsigned int sId, unsigned int remaining, RejectionReason reason)
        : submissionId(sId),
          remainingQuantity(remaining),
          fillStatus(FillStatus::NONE),
          status(SubmissionStatus::REJECTED),
          rejectionReason(reason) {}

    SubmissionResults SubmissionResults::rejected(unsigned int orderId, unsigned int remaining,
                                                  RejectionReason reason) {
        if (reason == RejectionReason::NONE) {
            throw std::invalid_argument("rejected submission without a rejection reason error");
        }
        return SubmissionResults(orderId, remaining, reason);
    }

    unsigned int SubmissionResults::getSubmissionId() const {
        return submissionId;
    }

    const std::vector<Trade>& SubmissionResults::getGeneratedTrades() const {
        return generatedTrades;
    }

    unsigned int SubmissionResults::getRemainingQuantity() const {
        return remainingQuantity;
    }

    FillStatus SubmissionResults::getFillStatus() const {
        return fillStatus;
    }

    SubmissionStatus SubmissionResults::getStatus() const {
        return status;
    }

    RejectionReason SubmissionResults::getRejectionReason() const {
        return rejectionReason;
    }
}