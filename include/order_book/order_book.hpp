#pragma once

#include <unordered_map>
#include <map>
#include <optional>
#include <compare>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "cancellation_results.hpp"
#include "order_location.hpp"
#include "price_level.hpp"
#include "order.hpp"
#include "submission_results.hpp"
#include "trade.hpp"
#include "types.hpp"

namespace order_book {

    struct PriceLevelSummary {
        Price price;
        unsigned int totalQuantity;
        unsigned int orderQuantity;
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

        std::unordered_map<OrderId, OrderLocation> orderIndex;

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

        std::vector<Trade> matchOrder(Order& incoming);

    public:
        OrderBook() = default;

        // Move-only: orderIndex holds iterators into this book's own level lists, so a
        // copy would point back into the original. Moving keeps those list nodes, and
        // with them the iterators, valid.
        OrderBook(const OrderBook&) = delete;
        OrderBook& operator=(const OrderBook&) = delete;
        OrderBook(OrderBook&&) = default;
        OrderBook& operator=(OrderBook&&) = default;

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
        // consumed once validation passes, even if matching later throws.
        void addOrder(Order order);

        SubmissionResults submitOrder(Order incoming);

        // Number of orders resting on the book (both sides), not of price levels.
        std::size_t activeOrderCount() const;

        // Copy of a resting order as it is now (later fills are not reflected); throws
        // std::invalid_argument if no order with that ID is resting.
        Order findActiveOrder(OrderId orderId) const;

        // Removes a resting order by ID, taking its remaining quantity off its level's
        // aggregate, unindexing it, and erasing the level once empty. Other orders keep
        // their FIFO positions and indexed iterators, and no sequence number or trade ID
        // is consumed. An ID that is not resting (unknown, already cancelled, fully filled,
        // or an incoming order that never rested) is rejected with ORDER_NOT_FOUND, leaving
        // the book unchanged.
        CancellationResult cancelOrder(OrderId orderId);

        // Fills resting orders at one price level in FIFO order, unindexing fully
        // filled orders and erasing the level once it is empty.
        ConsumptionResult consumeLevel(Side side, Price price, unsigned int quantity);

        BookSnapshot snapshot(unsigned int depth = 5) const;

    };
    
}  // namespace order_book
