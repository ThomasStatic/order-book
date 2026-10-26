#pragma once

#include <vector>
#include <stdexcept>

#include "book_event.hpp"
#include "trade.hpp"
#include "fill_status.hpp"
#include "rejection_reason.hpp"
#include "small_vector.hpp"

namespace order_book {

    enum class SubmissionStatus {
        ACCEPTED,
        REJECTED
    };

    // A submission's trades and events, held inline for up to four trades, so reporting one
    // allocates nothing in the common case: a submission that trades makes 1.2-2.1 trades on
    // average in the benchmark workloads (benchmarks/results/2026-10-26-profile/allocations.txt).
    using SubmissionTrades = SmallVector<Trade, 4>;
    using SubmissionEvents = SmallVector<BookEvent, 5>;  // the acceptance, then one per trade

    class SubmissionResults {
    private:
        unsigned int submissionId;
        
        SubmissionTrades generatedTrades;
        unsigned int remainingQuantity;
        
        FillStatus fillStatus;
        SubmissionStatus status;
        RejectionReason rejectionReason;

        SubmissionEvents events;

        // An accepted result with no trades; throws std::invalid_argument on a zero ID.
        explicit SubmissionResults(unsigned int sId);
        // Unchecked, so a rejection can echo whatever ID was submitted, including zero.
        SubmissionResults(unsigned int sId, unsigned int remaining, RejectionReason reason);

    public:

        // An accepted submission, identified by the incoming order's ID. Its fill status
        // follows from the trades and what is left: NONE if nothing traded, FILLED if
        // nothing is left, PARTIAL otherwise. Its events are the acceptance, then one
        // TradeExecuted per trade, in the trades' order. Throws std::invalid_argument on a
        // zero ID, quantity, or price, or unless every trade is for this order, on its side, and within its limit
        // price, and the traded quantities plus what is left sum to the accepted quantity.
        static SubmissionResults accepted(const OrderAccepted& acceptance, SubmissionTrades trades,
                                          unsigned int remaining);

        // A rejected submission: no trades, no fill, the submitted quantity left
        // unexecuted, and a single OrderRejected event. Throws std::invalid_argument if the
        // reason is NONE.
        static SubmissionResults rejected(unsigned int orderId, unsigned int remaining, RejectionReason reason);

        unsigned int getSubmissionId() const;

        const SubmissionTrades& getGeneratedTrades() const;
        unsigned int getRemainingQuantity() const;

        FillStatus getFillStatus() const;
        SubmissionStatus getStatus() const;
        RejectionReason getRejectionReason() const;

        const SubmissionEvents& getEvents() const;


    };
}  // order_book namespace