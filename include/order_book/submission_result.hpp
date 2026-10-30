#pragma once

#include "book_event.hpp"
#include "fill_status.hpp"
#include "rejection_reason.hpp"
#include "result_recorder.hpp"
#include "small_vector.hpp"
#include "trade.hpp"
#include "types.hpp"

namespace order_book {

    enum class SubmissionStatus {
        ACCEPTED,
        REJECTED
    };

    // Returned by OrderBook::submitOrder and addOrder. Built only through its factories and
    // ResultRecorder, which check what they are given, so its status, trades, fill status, and
    // events always agree. SubmissionTrades and SubmissionEvents are in result_recorder.hpp.
    class SubmissionResult {
    private:
        friend class ResultRecorder;

        OrderId submissionId;

        SubmissionTrades generatedTrades;
        Quantity remainingQuantity;

        FillStatus fillStatus;
        SubmissionStatus status;
        RejectionReason rejectionReason;

        SubmissionEvents events;

        // An accepted result, as ResultRecorder::finish has checked it.
        SubmissionResult(OrderId sId, SubmissionTrades&& trades, SubmissionEvents&& recordedEvents, Quantity remaining,
                         FillStatus fill);
        // Unchecked, so a rejection can echo whatever ID was submitted, including zero.
        SubmissionResult(OrderId sId, Quantity remaining, RejectionReason reason);

    public:
        // An accepted submission, identified by the incoming order's ID. Its fill status
        // follows from the trades and what is left: NONE if nothing traded, FILLED if
        // nothing is left, PARTIAL otherwise. Its events are the acceptance, then one
        // TradeExecuted per trade, in the trades' order. Throws std::invalid_argument on a
        // zero ID, quantity, or price, or unless every trade is for this order, on its side,
        // and within its limit price, and the traded quantities plus what is left sum to the
        // accepted quantity. Shorthand for a ResultRecorder fed only these trades.
        static SubmissionResult accepted(const OrderAccepted& acceptance, SubmissionTrades trades,
                                         Quantity remaining);

        // A rejected submission: no trades, no fill, the submitted quantity left
        // unexecuted, and a single OrderRejected event. Throws std::invalid_argument if the
        // reason is NONE.
        static SubmissionResult rejected(OrderId orderId, Quantity remaining, RejectionReason reason);

        OrderId getSubmissionId() const;

        const SubmissionTrades& getGeneratedTrades() const;
        Quantity getRemainingQuantity() const;

        FillStatus getFillStatus() const;
        SubmissionStatus getStatus() const;
        RejectionReason getRejectionReason() const;

        const SubmissionEvents& getEvents() const;
    };
}  // namespace order_book