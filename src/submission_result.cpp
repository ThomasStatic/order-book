#include <stdexcept>
#include <utility>

#include "order_book/submission_result.hpp"

namespace order_book {
    SubmissionResult::SubmissionResult(OrderId sId)
        : submissionId(sId),
          remainingQuantity(0),
          fillStatus(FillStatus::NONE),
          status(SubmissionStatus::ACCEPTED),
          rejectionReason(RejectionReason::NONE) {
        if (sId == 0) {
            throw std::invalid_argument("accepted result for the reserved order id 0 error");
        }
    }

    SubmissionResult SubmissionResult::accepted(const OrderAccepted& acceptance, SubmissionTrades trades,
                                                Quantity remaining) {
        if (acceptance.quantity == 0 || acceptance.price == 0) {
            throw std::invalid_argument("accepted result with no quantity or a zero price error");
        }
        // Summed wide so a bad set of trades cannot wrap around to look consistent.
        unsigned long long accounted = remaining;
        for (const Trade& trade : trades) {
            if (trade.getIncomingId() != acceptance.orderId || trade.getIncomingSide() != acceptance.side) {
                throw std::invalid_argument("accepted result with a trade for another incoming order error");
            }
            const bool withinLimit = acceptance.side == Side::BUY ? trade.getExecutionPrice() <= acceptance.price
                                                                  : trade.getExecutionPrice() >= acceptance.price;
            if (!withinLimit) {
                throw std::invalid_argument("accepted result with a trade beyond the order's limit price error");
            }
            accounted += trade.getExecutionQuantity();
        }
        if (accounted != acceptance.quantity) {
            throw std::invalid_argument("accepted result whose trades and remainder do not sum to its quantity error");
        }

        SubmissionResult result(acceptance.orderId);
        result.events.reserve(1 + trades.size());
        result.events.push_back(acceptance);
        for (const Trade& trade : trades) {
            result.events.push_back(TradeExecuted{trade.getIncomingId(), trade.getRestingId(),
                                                  trade.getExecutionPrice(), trade.getExecutionQuantity()});
        }
        result.fillStatus = trades.empty() ? FillStatus::NONE
                          : remaining == 0 ? FillStatus::FILLED
                                           : FillStatus::PARTIAL;
        result.generatedTrades = std::move(trades);
        result.remainingQuantity = remaining;
        return result;
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