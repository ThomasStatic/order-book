#include <algorithm>
#include <stdexcept>
#include <utility>

#include "order_book/replace_result.hpp"
#include "order_book/result_recorder.hpp"
#include "order_book/submission_result.hpp"

namespace order_book {
    ResultRecorder::ResultRecorder(const OrderAccepted& acceptance)
        : submissionId(acceptance.orderId),
          submittedQuantity(acceptance.quantity),
          current{acceptance.orderId, acceptance.side, acceptance.price, acceptance.quantity, false} {
        if (acceptance.orderId == 0) {
            throw std::invalid_argument("recorded result for the reserved order id 0 error");
        }
        if (acceptance.quantity == 0) {
            throw std::invalid_argument("recorded result for an order with no quantity error");
        }
        events.push_back(acceptance);
    }

    ResultRecorder::ResultRecorder(const OrderReplaced& replacement, Quantity executed)
        : submissionId(replacement.orderId),
          submittedQuantity(replacement.openQuantity),
          replacing(true),
          executedBefore(executed),
          current{replacement.orderId, replacement.side, replacement.price, replacement.openQuantity, false} {
        if (replacement.orderId == 0 || replacement.price == 0 || replacement.openQuantity == 0) {
            throw std::invalid_argument("recorded replace with the reserved id 0, no price, or nothing open error");
        }
        if (static_cast<unsigned long long>(replacement.openQuantity) + executed != replacement.quantity) {
            throw std::invalid_argument("recorded replace whose open and executed quantities do not sum to its total error");
        }
        events.push_back(replacement);
    }

    void ResultRecorder::reserve(std::size_t addedTrades, std::size_t addedEvents) {
        trades.reserve(trades.size() + addedTrades);
        events.reserve(events.size() + addedEvents);
    }

    void ResultRecorder::prevented(const SelfTradePrevented& prevention, Side incomingSide) {
        checkIncoming(prevention.incomingOrderId, incomingSide, prevention.price);
        events.push_back(prevention);
    }

    void ResultRecorder::expire(Quantity expiredQuantity) {
        checkOpen();
        if (current.expired) {
            throw std::invalid_argument("second expiry of one order error");
        }
        if (expiredQuantity == 0 || expiredQuantity != current.open) {
            throw std::invalid_argument("expiry other than the order's whole open quantity error");
        }
        events.push_back(OrderExpired{current.id, expiredQuantity});
        current.expired = true;
    }

    void ResultRecorder::closeSubmitted() noexcept {
        if (submittedCurrent) {
            submittedRemaining = current.open;
            submittedCurrent = false;
        }
    }

    void ResultRecorder::triggered(const OrderAccepted& stop, Price stopPrice, Price tradePrice) {
        checkOpen();
        if (stop.orderId == 0 || stop.quantity == 0) {
            throw std::invalid_argument("triggered stop with the reserved id 0 or no quantity error");
        }
        if (stopPrice == 0 || tradePrice == 0) {
            throw std::invalid_argument("triggered stop with no stop price or trade price error");
        }
        events.push_back(StopTriggered{stop.orderId, stopPrice, tradePrice});
        closeSubmitted();
        current = CurrentOrder{stop.orderId, stop.side, stop.price, stop.quantity, false};
    }

    void ResultRecorder::adoptTrades(SubmissionTrades adopted) {
        for (const Trade& adoptedTrade : adopted) {
            checkTrade(adoptedTrade);
            current.open -= adoptedTrade.getExecutionQuantity();
        }
        events.reserve(events.size() + adopted.size());
        for (const Trade& adoptedTrade : adopted) {
            events.push_back(TradeExecuted{adoptedTrade.getIncomingId(), adoptedTrade.getRestingId(), adoptedTrade.getExecutionPrice(),
                                           adoptedTrade.getExecutionQuantity()});
        }
        trades = std::move(adopted);
    }

    ReplaceResult ResultRecorder::finishReplace() {
        checkOpen();
        if (!replacing) {
            throw std::invalid_argument("finishing a submission's recorder as a replace error");
        }
        closeSubmitted();
        finished = true;
        const bool executed = executedBefore > 0 || submittedRemaining < submittedQuantity;
        const FillStatus fillStatus = !executed               ? FillStatus::NONE
                                    : submittedRemaining == 0 ? FillStatus::FILLED
                                                              : FillStatus::PARTIAL;
        return ReplaceResult(true, std::nullopt, std::move(trades), std::move(events), submittedRemaining, fillStatus);
    }

    SubmissionResult ResultRecorder::finish() {
        checkOpen();
        if (replacing) {
            throw std::invalid_argument("finishing a replace's recorder as a submission error");
        }
        closeSubmitted();
        finished = true;
        const FillStatus fillStatus = submittedRemaining == submittedQuantity ? FillStatus::NONE
                                    : submittedRemaining == 0                ? FillStatus::FILLED
                                                                             : FillStatus::PARTIAL;
        return SubmissionResult(submissionId, std::move(trades), std::move(events), submittedRemaining, fillStatus);
    }
}  // namespace order_book
