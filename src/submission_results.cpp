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

    unsigned int SubmissionResults::getSubmissionId() const {
        return submissionId;
    }

    std::vector<Trade> SubmissionResults::getGeneratedTrades() const {
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