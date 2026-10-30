#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "cancellation_result.hpp"
#include "invariants.hpp"
#include "node_pool.hpp"
#include "order.hpp"
#include "order_index.hpp"
#include "order_location.hpp"
#include "order_request.hpp"
#include "price_level.hpp"
#include "replace_request.hpp"
#include "replace_result.hpp"
#include "result_recorder.hpp"
#include "self_trade_prevention.hpp"
#include "side.hpp"
#include "submission_result.hpp"
#include "time_in_force.hpp"
#include "trade.hpp"
#include "types.hpp"

namespace order_book {

    // One level of a BookSnapshot.
    struct PriceLevelSummary {
        Price price;
        std::uint64_t totalQuantity;  // the level's aggregate, which can exceed one Quantity
        unsigned int orderCount;      // resting orders at this price
    };

    struct BookSnapshot {
        std::vector<PriceLevelSummary> bids;
        std::vector<PriceLevelSummary> asks;
        unsigned int totalActiveVisibleOrders;
    };

    class OrderBook final {
    private:
        // Test-only access to internals; defined in tests, never in the library.
        friend struct OrderBookTestAccess;

        // The blocks of every resting order's queue node. Declared first, so it is destroyed
        // after the levels whose nodes it holds; held by pointer, so its address, which every
        // queue's allocator records, survives moving the book. A moved-from book has none,
        // and any level it creates afterwards takes its nodes from the heap.
        std::unique_ptr<NodePool> nodePool = std::make_unique<NodePool>();

        OrderIndex orderIndex;
        // Every pending stop's ID and place. An ID is live while either index holds it.
        PendingStopIndex pendingIndex;

        std::map<Price, PriceLevel, std::greater<Price>> bids;
        std::map<Price, PriceLevel, std::less<Price>> asks;

        // Pending stops by stop price, so that on each side begin() is the stop nearest to
        // triggering: buy stops ascending (a rising trade reaches the lowest first), sell stops
        // descending. Each queue is in acceptance order.
        std::map<Price, StopLevel, std::less<Price>> buyStops;
        std::map<Price, StopLevel, std::greater<Price>> sellStops;

        // Stops a trade has triggered, waiting their turn in this command's cascade, oldest
        // first. Empty between commands. Same allocator as the stop levels, so a stop moves
        // here by splicing, without allocating. Created with the first pending stop, and held
        // by pointer: a list's own sentinel node comes from its allocator, so as a plain member
        // a moved-from book would keep a node in the pool the book it moved to now owns.
        std::unique_ptr<StopQueue> triggeredStops;

        // The price of the book's most recent trade: what stops trigger on.
        std::optional<Price> lastTradePrice;

        TradeId nextTradeId = 1;
        // Assigned to each order as the book accepts it, so it records acceptance order
        // and fixes time priority within a level.
        SequenceNum nextSequenceNum = 1;

        // Why the book must refuse this order, or nothing if it may be accepted; reads only,
        // so it runs before anything is mutated. Malformed fields are reported first, via
        // Order::malformedReason (checked again here because an order may be changed after
        // construction), then a FILLED order, then an ID already resting here.
        std::optional<RejectionReason> validateNewOrder(const Order& order) const;
        // The same for a request: its malformed checks in their fixed order, then an ID
        // already live here.
        std::optional<RejectionReason> validateNewRequest(const OrderRequest& request) const;
        // Whether an order rests or a stop pends under this ID.
        bool isLive(OrderId orderId) const;

        // Move to the next number, wrapping from the maximum back to 1 (zero is invalid),
        // so numbers are silently reused once a counter overflows.
        void advanceSequenceNum();
        void advanceTradeId();

        // Appends an already accepted order to the back of its price level and indexes
        // it, leaving the book unchanged if that throws. Throws std::runtime_error if its
        // ID is already indexed: callers must validate first.
        void restOrder(const Order& order, OwnerId owner = 0);
        // The level an index entry's handle names, checked to be a level at the entry's price
        // and side before anything is changed through it; throws std::runtime_error otherwise
        // (an engine bug).
        static PriceLevel& checkedLevel(const OrderLocation& location);

        // Takes a resting order off its level and out of the index, erasing the level once
        // empty: cancelOrder's removal, shared with self-trade prevention. Throws
        // std::runtime_error, before changing anything, if the entry's level handle does not
        // name a level at its price and side; after that check nothing can throw.
        void removeResting(OrderIndex::iterator entry);

        // What a submission says about how it may trade, beyond the order itself.
        struct Instructions {
            TimeInForce timeInForce = TimeInForce::GTC;
            OwnerId owner = 0;
            SelfTradePrevention selfTradePrevention = SelfTradePrevention::CANCEL_RESTING;
        };

        // Whether the order is at or through the best opposing price: an incoming order that
        // would take liquidity. A locked price counts; an empty opposite side never does. Shared
        // by addOrder, which refuses such an order, and post-only, which expires it.
        bool crossesBestOpposing(const Order& incoming) const;

        // How many trades and self-trade preventions a fill-or-kill order would make, or nothing
        // if the opposing orders within its limit cannot fill its whole quantity. Reads only.
        // Without an owner it sums level aggregates from the best level, then counts the orders
        // it would take at the last, so it costs O(levels crossed + orders taken at the last).
        // With one it visits every order it crosses, O(orders crossed), so a self-trade never
        // counts toward filling: under CANCEL_RESTING it skips (and counts) each own order, and
        // under the other modes it stops at the first.
        struct FillPlan {
            std::size_t trades;
            std::size_t preventions;
        };
        std::optional<FillPlan> fillOrKillPlan(const Order& incoming, const Instructions& how) const;

        Trade executeOnce(Order& incoming);

        // Matches while the incoming order has quantity and crosses, recording each trade. With
        // an owner, it first checks the oldest order at the best opposing level for the same
        // owner, and prevents that match as the mode says. Returns whether self-trade
        // prevention stopped the incoming order, whose remainder must then expire.
        bool matchOrder(Order& incoming, const Instructions& how, ResultRecorder& recorder);

        // submitOrder's steps once the order is accepted, shared by both overloads: number it,
        // match it, build the result, then rest any remainder (GTC) or expire it (IOC). A FOK
        // instead decides first whether it fills, makes room for its whole result, and only
        // then is numbered and matched, so it either completes or throws having changed nothing.
        // The steps below record into the command's ResultRecorder, which the command finishes
        // once its cascade has run.
        //
        // The Order a request runs as: an order with no price (market, or a triggered STOP) as a
        // limit at the furthest price on its side.
        Order incomingOrder(const OrderRequest& request) const;
        // runIncoming numbers the order, matches it, then rests its remainder (GTC) or expires
        // it (IOC, or stopped by self-trade prevention); a FOK goes to runFillOrKill, which
        // decides first whether it fills, makes room for all it records, and only then is
        // numbered and matched, so it either completes or throws having changed nothing.
        void runIncoming(Order& incoming, const Instructions& how, ResultRecorder& recorder);
        void runFillOrKill(Order& incoming, const Instructions& how, ResultRecorder& recorder);
        // Rests the order, or expires it whole if it would take. Strongly atomic and numbered
        // last, like addOrder, so a throw changes nothing.
        void runPostOnly(Order& incoming, OwnerId owner, ResultRecorder& recorder);

        // A STOP or STOP_LIMIT request: accepted and wholly expired if it is already past its
        // trigger, else stored pending. Strongly atomic and numbered last.
        void submitStop(const OrderRequest& request, ResultRecorder& recorder);

        // Moves every pending stop a trade at tradePrice triggers into triggeredStops: on one
        // side, nearest stop price first, acceptance order within a price. Unindexes each.
        // Allocates nothing; a trade that triggers nothing costs O(1).
        void triggerStops(Price tradePrice) noexcept;
        // Moves a resting order to the back of the level at newPrice (its own, if unchanged)
        // with the next sequence number, a new total and open quantity, and a new owner,
        // splicing its own list node. The one possible allocation, a new level, comes first, so
        // a throw changes nothing. Does not advance the counter.
        void moveToBack(OrderIndex::iterator entry, Price newPrice, Quantity total, Quantity open, OwnerId owner);

        // Runs the triggered stops one at a time, each as its market or limit order with a fresh
        // sequence number, appending what their trades trigger, until none is left.
        void runCascade(ResultRecorder& recorder);

        // Placed in each submission after its InvariantBoundary, so it runs first as an
        // exception unwinds: stops triggered but not yet run are discarded. They have already
        // left the pending set, so none is left pending past its trigger.
        class TriggeredStopsGuard {
        public:
            explicit TriggeredStopsGuard(OrderBook& owner) noexcept : book(owner) {}
            TriggeredStopsGuard(const TriggeredStopsGuard&) = delete;
            TriggeredStopsGuard& operator=(const TriggeredStopsGuard&) = delete;
            ~TriggeredStopsGuard() {
                if(book.triggeredStops) {
                    book.triggeredStops->clear();
                }
            }

        private:
            OrderBook& book;
        };
        // Whether a trade at the last trade price would already have triggered this stop.
        bool isPastTrigger(const OrderRequest& stop) const noexcept;
        // Queues a stop at the back of its stop level with the next sequence number and indexes
        // it, leaving the book unchanged if that throws. Does not advance the counter.
        void storePendingStop(const OrderRequest& stop);
        // Takes a pending stop off its stop level and out of the index, erasing the level once
        // empty. Throws std::runtime_error, before changing anything, if the entry's level
        // handle does not name a stop level at its stop price and side.
        void removePendingStop(PendingStopIndex::iterator entry);

        // Fills resting orders at one price level in FIFO order, unindexing fully filled
        // orders and erasing the level once it is empty. It makes no trades and reports no
        // events, so it is not a public command; tests reach it through OrderBookTestAccess.
        // Throws std::invalid_argument if the side has no level at that price.
        ConsumptionResult consumeLevel(Side side, Price price, Quantity quantity);

        // Placed at the top of each command that can change the book (the public ones and
        // consumeLevel), so the whole book is checked once the command has finished, whether
        // it returns or throws. That is the invariant boundary: steps inside a command may
        // leave the structures out of step with each other for a moment. Does nothing unless
        // invariantChecksAfterCommands.
        class InvariantBoundary {
        public:
            InvariantBoundary(const OrderBook& checkedBook, const char* commandName)
                : book(checkedBook), command(commandName) {}
            InvariantBoundary(const InvariantBoundary&) = delete;
            InvariantBoundary& operator=(const InvariantBoundary&) = delete;
            ~InvariantBoundary() {
                if constexpr(invariantChecksAfterCommands) {
                    book.enforceInvariants(command);
                }
            }

        private:
            const OrderBook& book;
            const char* command;
        };

        // Runs checkInvariants and, on a violation, prints it with the command's name to
        // stderr and calls std::abort: a broken invariant is an engine bug, not an error a
        // caller could handle.
        void enforceInvariants(const char* command) const noexcept;

        // The steps of checkInvariants, each returning the first violation it finds.
        // checkLevels covers one side's levels and orders on their own, counting the orders;
        // checkIndexedLevels then checks each of that side's orders against the index.
        template <typename Levels>
        InvariantResult checkLevels(const Levels& levels, Side side, std::size_t& restingOrders) const noexcept;
        template <typename Levels>
        InvariantResult checkIndexedLevels(const Levels& levels, Side side) const noexcept;
        // The same two for one side's pending stops.
        template <typename StopLevels>
        InvariantResult checkStopLevels(const StopLevels& levels, Side side, std::size_t& pendingStops) const noexcept;
        template <typename StopLevels>
        InvariantResult checkIndexedStops(const StopLevels& levels, Side side) const noexcept;
        InvariantResult findStalePendingEntry(std::size_t pendingStops) const noexcept;
        // Finds an index entry that refers to no resting order; called once the index is known
        // to hold more entries than there are resting orders.
        InvariantResult findStaleIndexEntry(std::size_t restingOrders) const noexcept;
        // Whether a resting order other than `order` has its ID.
        bool idRestsElsewhere(const Order& order) const noexcept;
        // How many numbers ago this one was issued, counting back from nextSequenceNum, so an
        // order accepted later has a smaller age even across a wrap of the counter.
        std::uint64_t sequenceAge(SequenceNum sequenceNum) const noexcept;

    public:
        OrderBook() = default;

        // Move-only: orderIndex holds iterators into this book's own level lists, so a
        // copy would point back into the original. Moving keeps those list nodes, and
        // with them the iterators, valid.
        //
        // Memory: resting orders' queue nodes come from a pool the book owns and reuses, so a
        // book keeps the node memory of the most orders it has held until it is destroyed;
        // its index likewise keeps the table it grew to. Only levels use the heap and are
        // freed as they go. Moving a book moves its pool; a moved-from book has none, and any
        // order it rests later takes its node from the heap.
        OrderBook(const OrderBook&) = delete;
        OrderBook& operator=(const OrderBook&) = delete;
        OrderBook(OrderBook&&) = default;
        // Not defaulted: this book's levels must release their nodes into its own pool before
        // that pool is replaced by the other book's. Assigns every member by hand, so a new
        // member must be added here too.
        OrderBook& operator=(OrderBook&& other) noexcept;

        // Copy of the oldest order at the best price; throws std::runtime_error if that
        // side is empty. A copy cannot dangle once the order is filled or removed.
        Order getBestBid() const;
        Order getBestAsk() const;

        bool hasBids() const;
        bool hasAsks() const;

        // Both accept the order with the book's next sequence number, replacing the one it
        // was constructed with. addOrder rests it without matching and consumes no number
        // if it throws. submitOrder matches it by price-time priority, reporting trades in
        // execution order, and rests any remainder at its own limit price; its number is
        // consumed once validation passes, even if matching later throws. Both return the
        // submission's events; addOrder's are only its OrderAccepted, and it throws
        // std::invalid_argument on a rejection rather than returning one. Since addOrder
        // never matches, it also refuses an order that would cross the best opposing price
        // (WOULD_CROSS_BOOK), so the book is never left crossed. It is the setup and seeding
        // command; a post-only submitOrder(OrderRequest) is the version that reports a would-be
        // taker through its events instead.
        //
        // With invariantChecksAfterCommands set, every command below that can change the book
        // also checks it when it returns or throws, and aborts on a broken invariant. So an
        // exception caused by the book being internally inconsistent (not a failed allocation,
        // which leaves it sound) aborts there instead of reaching the caller.
        SubmissionResult addOrder(Order order);

        SubmissionResult submitOrder(Order incoming);

        // Submits a request. It is checked first, and a malformed one (by the first failing
        // check: a zero ID, a zero price other than on a MARKET order, a zero quantity, an
        // invalid side, a type that is not an enumerator (UNSUPPORTED_ORDER_TYPE), a price on a
        // MARKET or STOP order (PRICE_NOT_ALLOWED), a time in force that is not an enumerator,
        // a MARKET or STOP order with GTC (TIME_IN_FORCE_NOT_ALLOWED), a stop with no stop price
        // (INVALID_STOP_PRICE), a stop price on a type that is not a stop
        // (STOP_PRICE_NOT_ALLOWED), post-only on anything but a GTC LIMIT order, then an invalid
        // self-trade prevention mode) or one whose ID is live (resting or pending) is a REJECTED
        // result that changes nothing. A STOP or STOP_LIMIT order already past its trigger (a buy
        // stop at or below the last trade price, a sell stop at or above it) is accepted and
        // wholly expired; otherwise it pends, its result holding only its OrderAccepted (at its
        // limit price, or 0 for a STOP), strongly atomic and numbered last. Before the first
        // trade no stop is past its trigger. A MARKET order has no price protection: it trades
        // at each resting price, as far through the opposite side as its quantity takes it, and
        // its acceptance reports price 0; as IOC or FOK, its remainder expires. A post-only
        // order (GTC LIMIT only, else POST_ONLY_NOT_ALLOWED) rests like any limit order, or, if
        // it would take liquidity (at or through the best opposing price), is accepted and
        // wholly expired without trading or allocating; it is strongly atomic and numbered last.
        // Self-trade prevention: when the incoming order has a nonzero owner and would trade
        // next with a resting order of the same owner, the incoming order's mode decides
        // (CANCEL_RESTING removes the resting order whole and goes on matching; CANCEL_INCOMING
        // leaves it and stops, expiring the remainder even of a GTC order; CANCEL_BOTH does
        // both), reported as a SelfTradePrevented among the trades, in execution order. A
        // prevention is never a trade: it consumes no trade ID. A FOK never counts a self-trade
        // toward filling, and one that cannot fill cancels nothing. Post-only never matches, so
        // it never runs it. An invalid mode is INVALID_SELF_TRADE_PREVENTION whatever the owner. A well-formed request is always
        // accepted and matched like submitOrder(Order); then a GTC remainder rests, and an IOC
        // remainder expires instead (a trailing OrderExpired for the whole remainder, which still
        // counts as the result's remaining quantity). An IOC that does not cross, or faces an
        // empty side, is accepted and wholly expired; either way it consumes a sequence number
        // and never rests. A FOK fills its whole quantity, or else makes no trades and wholly
        // expires (FillStatus NONE, no trade ID consumed, the book otherwise untouched; this
        // allocates nothing). It is strongly atomic: if an allocation fails, it throws with the
        // book and its counters unchanged. A request with every field after quantity at its default behaves
        // exactly as submitOrder(Order) does for an Order with the same ID, side, price, and
        // quantity, and allocates no more.
        SubmissionResult submitOrder(OrderRequest request);

        // Replaces a resting order under the same ID with a new price, total quantity (including
        // what has traded), and instructions, which are all declared again. Refused, changing
        // nothing and allocating nothing, as one ReplaceRejected, by the first failing check: a
        // zero ID, price, or quantity, an invalid self-trade prevention mode, a pending stop's ID
        // (ORDER_NOT_REPLACEABLE: move a stop by cancelling and submitting again), an ID not
        // resting (ORDER_NOT_FOUND), then a total at or below what has executed
        // (REPLACE_QUANTITY_TOO_LOW: a replace never cancels). Otherwise it reports OrderReplaced
        // (new price, total, open quantity = total - executed, priority kept or not) and:
        // - at the same price with no increase, it keeps its time priority: done in place,
        //   keeping its queue position and sequence number; it never crosses, allocates nothing,
        //   and cannot throw once found;
        // - otherwise it loses priority, re-entering as if it had just arrived, under the new
        //   instructions. Post-only at a price that would take, it leaves the book, its whole
        //   open quantity expired (no allocation). Not crossing, it moves to the back of the
        //   queue at its new price with a new sequence number, strongly atomic and numbered last.
        //   Crossing, it leaves its level and matches as an incoming GTC limit order under the
        //   new mode, with submitOrder's (non-atomic) contract: its trades set the last trade
        //   price and may trigger stops, which run as a cascade reported in the result.
        // The order's initial quantity becomes the new total, so a partly filled order stays
        // PARTIAL. Side, type, time in force, and stop price never change.
        ReplaceResult replaceOrder(const ReplaceRequest& request);

        // Number of orders resting on the book (both sides), not of price levels. Pending stops
        // are not resting orders.
        std::size_t activeOrderCount() const;

        // Number of stops pending, both sides. O(1).
        std::size_t pendingStopCount() const;

        // The request a pending stop was accepted with, every field as submitted, or nothing if
        // no stop pends under this ID.
        std::optional<OrderRequest> findPendingStop(OrderId orderId) const;

        // The price of the book's most recent trade, which is what stops trigger on: set by
        // every trade, and by nothing else (not a prevented self-trade, an expiry, addOrder, or
        // cancelOrder). Empty before the first trade; never reset; moves with the book.
        std::optional<Price> getLastTradePrice() const noexcept;

        // Sizes the order index for at least `orders` resting orders, so it does not have to
        // grow, rehashing every entry, until the book holds more. (Pending stops have an index
        // of their own, which this does not size.) A capacity hint only: it
        // changes nothing the book reports, does no matching, and consumes no sequence number
        // or trade ID. Throws std::bad_alloc, or std::length_error for a hint beyond what the
        // index can address, leaving it unchanged.
        // An oversized hint costs memory and, once the book is larger than the cache, some
        // lookup speed, so pass the size the book is expected to reach, not a loose bound.
        void reserve(std::size_t orders);

        // Copy of a resting order as it is now (later fills are not reflected); throws
        // std::invalid_argument if no order with that ID is resting (a pending stop's included).
        Order findActiveOrder(OrderId orderId) const;

        // Removes a resting order or a pending stop by ID. A pending stop's cancellation reports
        // its whole quantity, and its stop level is erased once empty. A resting order's takes
        // its remaining quantity off its level's
        // aggregate, unindexing it, and erasing the level once empty. Other orders keep
        // their FIFO positions and indexed iterators, and no sequence number or trade ID
        // is consumed. An ID that is not resting (unknown, already cancelled, fully filled,
        // or an incoming order that never rested) is rejected with ORDER_NOT_FOUND, leaving
        // the book unchanged. Throws std::runtime_error, before changing anything, if an
        // indexed order's level, or a pending stop's stop level, is missing.
        CancellationResult cancelOrder(OrderId orderId);

        // The best `depth` levels of each side, best price first, with each level's aggregate
        // and order count. Pending stops offer no liquidity, so they are in no snapshot.
        BookSnapshot snapshot(unsigned int depth = 5) const;

        // Checks the whole book in one O(N + L) pass over its N resting orders and L levels,
        // stopping at the first broken invariant: each side's levels ordered best price
        // first, non-empty, and holding their own price and side; each resting order well
        // formed, with quantity left, at its level's price and side, and queued in
        // acceptance order; each level's aggregate equal to its orders' remaining sum;
        // exactly one index entry per resting order, recording its side, price, level, and
        // list position, with no ID resting twice; and best bid strictly below best ask.
        //
        // Never allocates or throws, and never dereferences an index entry's iterator. It
        // compares that iterator with the list's own only after the entry has named the
        // right side and price. An entry left behind by a removed order is found by its
        // key alone. So a stale entry is detected safely, while an entry for a resting ID
        // that holds an iterator to an erased element is still undefined to compare (MSVC's
        // checked iterators stop the program there). Sequence numbers are compared relative
        // to the counter, so a level queued across a wrap still passes. That holds as long
        // as no order rests through a full cycle of 2^32 - 1 acceptances.
        //
        // Callable in every build. Public commands also run it on return when
        // invariantChecksAfterCommands is set (see invariants.hpp).
        InvariantResult checkInvariants() const noexcept;
    };
}  // namespace order_book
