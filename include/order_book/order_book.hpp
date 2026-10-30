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
#include "side.hpp"
#include "submission_result.hpp"
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

        std::map<Price, PriceLevel, std::greater<Price>> bids;
        std::map<Price, PriceLevel, std::less<Price>> asks;

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

        // Move to the next number, wrapping from the maximum back to 1 (zero is invalid),
        // so numbers are silently reused once a counter overflows.
        void advanceSequenceNum();
        void advanceTradeId();

        // Appends an already accepted order to the back of its price level and indexes
        // it, leaving the book unchanged if that throws. Throws std::runtime_error if its
        // ID is already indexed: callers must validate first.
        void restOrder(const Order& order);

        bool crossesBestOpposing(const Order& incoming) const;

        Trade executeOnce(Order& incoming);

        SubmissionTrades matchOrder(Order& incoming);

        // submitOrder's steps once the order is accepted, shared by both overloads: number it,
        // match it, build the result, and rest any remainder.
        SubmissionResult matchAndRest(Order& incoming);

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
        // (WOULD_CROSS_BOOK), so the book is never left crossed.
        //
        // With invariantChecksAfterCommands set, every command below that can change the book
        // also checks it when it returns or throws, and aborts on a broken invariant. So an
        // exception caused by the book being internally inconsistent (not a failed allocation,
        // which leaves it sound) aborts there instead of reaching the caller.
        SubmissionResult addOrder(Order order);

        SubmissionResult submitOrder(Order incoming);

        // Submits a request. It is checked first, and a malformed one (by the first failing
        // check: a zero ID, a zero price, a zero quantity, an invalid side, then a type that
        // is not an enumerator; only LIMIT is accepted for now, and any other type is
        // UNSUPPORTED_ORDER_TYPE) or one whose ID is live is a REJECTED result that changes
        // nothing. A request with every field after quantity at its default behaves exactly
        // as submitOrder(Order) does for an Order with the same ID, side, price, and quantity,
        // and allocates no more.
        SubmissionResult submitOrder(OrderRequest request);

        // Number of orders resting on the book (both sides), not of price levels.
        std::size_t activeOrderCount() const;

        // Sizes the order index for at least `orders` resting orders, so it does not have to
        // grow, rehashing every entry, until the book holds more. A capacity hint only: it
        // changes nothing the book reports, does no matching, and consumes no sequence number
        // or trade ID. Throws std::bad_alloc, or std::length_error for a hint beyond what the
        // index can address, leaving it unchanged.
        // An oversized hint costs memory and, once the book is larger than the cache, some
        // lookup speed, so pass the size the book is expected to reach, not a loose bound.
        void reserve(std::size_t orders);

        // Copy of a resting order as it is now (later fills are not reflected); throws
        // std::invalid_argument if no order with that ID is resting.
        Order findActiveOrder(OrderId orderId) const;

        // Removes a resting order by ID, taking its remaining quantity off its level's
        // aggregate, unindexing it, and erasing the level once empty. Other orders keep
        // their FIFO positions and indexed iterators, and no sequence number or trade ID
        // is consumed. An ID that is not resting (unknown, already cancelled, fully filled,
        // or an incoming order that never rested) is rejected with ORDER_NOT_FOUND, leaving
        // the book unchanged. Throws std::runtime_error, before changing anything, if an
        // indexed order's level is missing.
        CancellationResult cancelOrder(OrderId orderId);

        // The best `depth` levels of each side, best price first, with each level's aggregate
        // and order count.
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
