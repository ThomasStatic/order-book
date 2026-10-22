#pragma once

#include <vector>
#include <stdexcept>

#include "book_event.hpp"
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

        std::vector<BookEvent> events;

        // An accepted result with no trades; throws std::invalid_argument on a zero ID.
        explicit SubmissionResults(unsigned int sId);
        // Unchecked, so a rejection can echo whatever ID was submitted, including zero.
        SubmissionResults(unsigned int sId, unsigned int remaining, RejectionReason reason);

    public:

        // An accepted submission, identified by the incoming order's ID. Its fill status
        // follows from the trades and what is left: NONE if nothing traded, FILLED if
        // nothing is left, PARTIAL otherwise. Its events are the acceptance, then one
        // TradeExecuted per trade, in the trades' order. Throws std::invalid_argument on a
        // zero ID.
        static SubmissionResults accepted(const OrderAccepted& acceptance, std::vector<Trade> trades,
                                          unsigned int remaining);

        // A rejected submission: no trades, no fill, the submitted quantity left
        // unexecuted, and a single OrderRejected event. Throws std::invalid_argument if the
        // reason is NONE.
        static SubmissionResults rejected(unsigned int orderId, unsigned int remaining, RejectionReason reason);

        unsigned int getSubmissionId() const;

        const std::vector<Trade>& getGeneratedTrades() const;
        unsigned int getRemainingQuantity() const;

        FillStatus getFillStatus() const;
        SubmissionStatus getStatus() const;
        RejectionReason getRejectionReason() const;

        const std::vector<BookEvent>& getEvents() const;


    };
}  // order_book namespace