#pragma once

#include <cstddef>

#include "book_event.hpp"
#include "side.hpp"
#include "small_vector.hpp"
#include "trade.hpp"
#include "types.hpp"

namespace order_book {

    class SubmissionResult;

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

        // The result. The recorder is spent afterwards: any further call throws
        // std::invalid_argument.
        SubmissionResult finish();

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

        CurrentOrder current;
    };

}  // namespace order_book
