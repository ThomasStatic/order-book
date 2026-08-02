#pragma once

#include <vector>
#include <stdexcept>

#include "trade.hpp"
#include "fill_status.hpp"
#include "rejection_reason.hpp"

namespace order_book {

    enum class SubmissionStatus {
        ACCEPTED,
        REJECTED
    };

    class SubmissionResults {
    private:
        unsigned int submissionId;
        
        std::vector<Trade> generatedTrades;
        unsigned int remainingQuantity;
        
        FillStatus fillStatus;
        SubmissionStatus status;
        RejectionReason rejectionReason;

    public:
        SubmissionResults(unsigned int sId);

        unsigned int getSubmissionId() const;

        std::vector<Trade> getGeneratedTrades() const;
        unsigned int getRemainingQuantity() const;

        FillStatus getFillStatus() const;
        SubmissionStatus getStatus() const;
        RejectionReason getRejectionReason() const;


    };
}  // order_book namespace