#pragma once

#include <optional>

#include "book_event.hpp"
#include "fill_status.hpp"
#include "rejection_reason.hpp"
#include "result_recorder.hpp"
#include "types.hpp"

namespace order_book {

    // Returned by OrderBook::replaceOrder. Built only through ResultRecorder, opened by the
    // order's OrderReplaced, and through rejected(), so its outcome, events, and quantities
    // always agree. It uses a submission's trade and event storage.
    class ReplaceResult {
    private:
        friend class ResultRecorder;

        bool success;
        std::optional<RejectionReason> rejectionReason;
        SubmissionTrades generatedTrades;
        SubmissionEvents events;
        Quantity remainingQuantity;
        FillStatus fillStatus;

        ReplaceResult(bool succeeded, std::optional<RejectionReason> reason, SubmissionTrades trades,
                      SubmissionEvents recordedEvents, Quantity remaining, FillStatus fill);

    public:
        // A refused replace: one ReplaceRejected, no trades, nothing remaining, FillStatus NONE.
        // Unchecked ID, so it can echo whatever was asked for, including zero. Throws
        // std::invalid_argument if the reason is NONE.
        static ReplaceResult rejected(OrderId orderId, RejectionReason reason);

        bool isSuccess() const;
        std::optional<RejectionReason> getRejectionReason() const;
        // OrderReplaced, then the order's trades and preventions, any OrderExpired, and any
        // cascade its trades triggered, in execution order.
        const SubmissionEvents& getEvents() const;
        const SubmissionTrades& getGeneratedTrades() const;
        // What the order has left to trade at the end of its own match (resting, or expired),
        // and its fill status over its whole life: a partly filled order stays PARTIAL.
        Quantity getRemainingQuantity() const;
        FillStatus getFillStatus() const;
    };

}  // namespace order_book
