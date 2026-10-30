#pragma once

#include <algorithm>
#include <cstddef>
#include <stdexcept>

#include "book_event.hpp"
#include "side.hpp"
#include "small_vector.hpp"
#include "trade.hpp"
#include "types.hpp"

namespace order_book {

    class SubmissionResult;
    class ReplaceResult;

    // A submission's trades and events, held inline for up to four trades and six events, so
    // reporting one allocates nothing in the common case: a submission that trades makes
    // 1.2-2.1 trades on average in the benchmark workloads
    // (benchmarks/results/2026-10-26-profile/allocations.txt). Six events is the acceptance,
    // four trades, and an expiry.
    inline constexpr std::size_t inlineSubmissionTrades = 4;
    inline constexpr std::size_t inlineSubmissionEvents = 6;
    using SubmissionTrades = SmallVector<Trade, inlineSubmissionTrades>;
    using SubmissionEvents = SmallVector<BookEvent, inlineSubmissionEvents>;

    // Builds a command's result as the book makes it: the book appends each trade and event
    // as it happens, and finish() yields the immutable result. Each append is checked as it
    // is made against the order it belongs to, the current order: the submitted one, or the
    // stop that triggered last. A check that fails throws std::invalid_argument, since it
    // means an engine bug, and leaves the recorder as it was.
    //
    // The result's events are the whole stream in execution order; its trades are every trade
    // the command made, cascade included; a prevention appears only as its event. Its
    // remaining quantity and fill status describe the submitted order at the end of its own
    // match, before any stop runs: remaining is what the submitted order did not trade,
    // whether it then rested or expired.
    //
    // Implementation detail, like SmallVector: public so it can be tested directly, and it may
    // change in any release.
    class ResultRecorder {
    public:
        // Opens the submitted order's part of the result with its acceptance. price 0 means
        // the order has no limit price. Throws std::invalid_argument on a zero ID or quantity.
        explicit ResultRecorder(const OrderAccepted& acceptance);
        // Opens a replace's result with its OrderReplaced: the replaced order is the current one,
        // with its open quantity to trade; executedBefore is what it had traded before the
        // replace. Throws std::invalid_argument on a zero ID, price, or open quantity, or unless
        // the new total is the open quantity plus executedBefore.
        ResultRecorder(const OrderReplaced& replacement, Quantity executedBefore);

        // Appends a trade and its TradeExecuted. Refused unless the trade's incoming order is
        // the current order, on its side (so the resting order is on the other), at a price
        // within its limit, and for no more than it has open.
        void trade(const Trade& executed);

        // Appends a self-trade prevention, never a trade. Refused unless its incoming order is
        // the current order, on incomingSide, the current order's side, at a price within its
        // limit.
        void prevented(const SelfTradePrevented& prevention, Side incomingSide);

        // Appends the current order's OrderExpired: the whole quantity it has open, which must
        // be positive. Nothing more of that order may follow.
        void expire(Quantity expiredQuantity);

        // Ends the current order's part and opens a triggered stop's with its StopTriggered.
        // stop describes the stop as it runs: its ID, side, limit price (0 for a STOP, which
        // runs as a market order), and quantity. Throws std::invalid_argument on a zero ID,
        // quantity, stop price, or trade price.
        void triggered(const OrderAccepted& stop, Price stopPrice, Price tradePrice);

        // Makes room for this many more trades and events, doubling, so that many appends cannot
        // throw for lack of memory. The book calls it before each step that changes the book,
        // so recording what the step did never fails once the book has changed. Throws
        // std::bad_alloc, leaving the recorder unchanged.
        void makeRoom(std::size_t addedTrades, std::size_t addedEvents);

        // Makes room for exactly this many more trades and events, so a match known in advance
        // to make that many cannot fail to record them. A fill-or-kill order calls it before it
        // is numbered. Allocates only beyond the capacity it has; throws std::bad_alloc, leaving
        // what has been recorded unchanged.
        void reserve(std::size_t addedTrades, std::size_t addedEvents);

        // The result. The recorder is spent afterwards: any further call throws
        // std::invalid_argument. finish() is for a recorder opened by an acceptance,
        // finishReplace() for one opened by a replacement; the other throws.
        SubmissionResult finish();
        ReplaceResult finishReplace();

    private:
        friend class SubmissionResult;

        // The order the next trade, prevention, or expiry belongs to.
        struct CurrentOrder {
            OrderId id;
            Side side;
            Price limitPrice;  // 0: none
            Quantity open;     // what it has left to trade or expire
            bool expired;
        };

        void checkOpen() const;
        void checkIncoming(OrderId incomingId, Side incomingSide, Price price) const;
        void checkTrade(const Trade& checked) const;
        // Ends the submitted order's part, fixing its remaining quantity, if it is current.
        void closeSubmitted() noexcept;

        // SubmissionResult::accepted's shorthand: checks and records trades as if each were
        // appended in turn, but takes the vector itself, so it allocates no more than the
        // trades already have.
        void adoptTrades(SubmissionTrades trades);

        SubmissionTrades trades;
        SubmissionEvents events;

        OrderId submissionId;
        Quantity submittedQuantity;
        Quantity submittedRemaining = 0;
        bool submittedCurrent = true;
        bool finished = false;
        bool replacing = false;
        Quantity executedBefore = 0;  // a replaced order's, before the replace

        CurrentOrder current;
    };

    // The per-trade path is defined here, inline, since the book calls it once per trade from
    // another translation unit (docs/OPTIMIZATION.md, the v2.0.0 regression).

    inline void ResultRecorder::checkOpen() const {
        if (finished) {
            throw std::invalid_argument("append to a finished result recorder error");
        }
    }

    inline void ResultRecorder::checkIncoming(OrderId incomingId, Side incomingSide, Price price) const {
        checkOpen();
        if (incomingId != current.id || incomingSide != current.side) {
            throw std::invalid_argument("recorded execution for an order other than the current one error");
        }
        if (current.expired) {
            throw std::invalid_argument("recorded execution after the order expired error");
        }
        // A limit price of 0 means none: a market order.
        const bool withinLimit = current.limitPrice == 0 ||
                                 (current.side == Side::BUY ? price <= current.limitPrice : price >= current.limitPrice);
        if (!withinLimit) {
            throw std::invalid_argument("recorded execution beyond the order's limit price error");
        }
    }

    inline void ResultRecorder::checkTrade(const Trade& checked) const {
        checkIncoming(checked.getIncomingId(), checked.getIncomingSide(), checked.getExecutionPrice());
        if (checked.getExecutionQuantity() > current.open) {
            throw std::invalid_argument("recorded trade for more than the order has open error");
        }
    }

    inline void ResultRecorder::makeRoom(std::size_t addedTrades, std::size_t addedEvents) {
        // Trades first: if events then fail to grow, the larger trade storage is harmless.
        // Doubling keeps repeated appends amortized O(1).
        if (trades.size() + addedTrades > trades.capacity()) {
            trades.reserve(std::max(trades.size() + addedTrades, trades.capacity() * 2));
        }
        if (events.size() + addedEvents > events.capacity()) {
            events.reserve(std::max(events.size() + addedEvents, events.capacity() * 2));
        }
    }

    inline void ResultRecorder::trade(const Trade& executed) {
        checkTrade(executed);
        // Room for both first, so neither push can throw once the other has been made.
        makeRoom(1, 1);
        trades.push_back(executed);
        events.push_back(TradeExecuted{executed.getIncomingId(), executed.getRestingId(), executed.getExecutionPrice(),
                                       executed.getExecutionQuantity()});
        current.open -= executed.getExecutionQuantity();
    }

}  // namespace order_book
