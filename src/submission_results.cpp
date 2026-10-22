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

    SubmissionResults SubmissionResults::accepted(const OrderAccepted& acceptance, std::vector<Trade> trades,
                                                  unsigned int remaining) {
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

        SubmissionResults result(acceptance.orderId);
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

    SubmissionResults::SubmissionResults(unsigned int sId, unsigned int remaining, RejectionReason reason)
        : submissionId(sId),
          remainingQuantity(remaining),
          fillStatus(FillStatus::NONE),
          status(SubmissionStatus::REJECTED),
          rejectionReason(reason),
          events{OrderRejected{sId, reason}} {}

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

    const std::vector<BookEvent>& SubmissionResults::getEvents() const {
        return events;
    }
}