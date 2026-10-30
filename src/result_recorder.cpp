#include <algorithm>
#include <stdexcept>
#include <utility>

#include "order_book/result_recorder.hpp"
#include "order_book/submission_result.hpp"

namespace order_book {
    namespace {
        bool withinLimit(Side side, Price limitPrice, Price price) {
            if (limitPrice == 0) {
                return true;  // no limit price: a market order
            }
            return side == Side::BUY ? price <= limitPrice : price >= limitPrice;
        }

        // Room for `added` more elements, doubling so repeated appends stay amortized O(1).
        template <typename Vector>
        void makeRoom(Vector& vector, std::size_t added) {
            const std::size_t wanted = vector.size() + added;
            if (wanted > vector.capacity()) {
                vector.reserve(std::max(wanted, vector.capacity() * 2));
            }
        }
    }  // namespace

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

    void ResultRecorder::checkOpen() const {
        if (finished) {
            throw std::invalid_argument("append to a finished result recorder error");
        }
    }

    void ResultRecorder::checkIncoming(OrderId incomingId, Side incomingSide, Price price) const {
        checkOpen();
        if (incomingId != current.id || incomingSide != current.side) {
            throw std::invalid_argument("recorded execution for an order other than the current one error");
        }
        if (current.expired) {
            throw std::invalid_argument("recorded execution after the order expired error");
        }
        if (!withinLimit(current.side, current.limitPrice, price)) {
            throw std::invalid_argument("recorded execution beyond the order's limit price error");
        }
    }

    void ResultRecorder::checkTrade(const Trade& checked) const {
        checkIncoming(checked.getIncomingId(), checked.getIncomingSide(), checked.getExecutionPrice());
        if (checked.getExecutionQuantity() > current.open) {
            throw std::invalid_argument("recorded trade for more than the order has open error");
        }
    }

    void ResultRecorder::reserveEvents(std::size_t added) {
        makeRoom(events, added);
    }

    void ResultRecorder::trade(const Trade& executed) {
        checkTrade(executed);
        // Room for both first, so neither push can throw once the other has been made.
        makeRoom(trades, 1);
        reserveEvents(1);
        trades.push_back(executed);
        events.push_back(TradeExecuted{executed.getIncomingId(), executed.getRestingId(), executed.getExecutionPrice(),
                                       executed.getExecutionQuantity()});
        current.open -= executed.getExecutionQuantity();
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

    SubmissionResult ResultRecorder::finish() {
        checkOpen();
        closeSubmitted();
        finished = true;
        const FillStatus fillStatus = submittedRemaining == submittedQuantity ? FillStatus::NONE
                                    : submittedRemaining == 0                ? FillStatus::FILLED
                                                                             : FillStatus::PARTIAL;
        return SubmissionResult(submissionId, std::move(trades), std::move(events), submittedRemaining, fillStatus);
    }
}  // namespace order_book
