#pragma once

#include <cstdint>
#include <vector>

#include "node_pool.hpp"
#include "order.hpp"
#include "order_location.hpp"
#include "side.hpp"
#include "types.hpp"

namespace order_book {

    // How OrderBook::consumeLevel left the level. SATISFIED means the level is now empty
    // (and erased); LEVEL_EXHAUSTED means quantity still rests there.
    enum class QuantityConsumptionStatus {
        SATISFIED,
        LEVEL_EXHAUSTED,
        INVALID_REQUEST  // a request for zero
    };

    struct OrderFill {
        OrderId orderId;
        Quantity filledQuantity;
        bool fullyFilled;  // order was removed from the level
    };

    struct ConsumptionResult {
        QuantityConsumptionStatus status;
        Quantity unfilledQuantity;     // requested quantity the level could not supply
        std::vector<OrderFill> fills;  // in FIFO order
    };

    // One price on one side of a book: a FIFO queue of resting orders and their cached
    // aggregate. An implementation detail of OrderBook, which keeps the aggregate, the queue,
    // and its index in step; the book never hands a level out.
    class PriceLevel {
    private:
        // OrderBook alone may fill a single order, since it must also unindex the order
        // and erase the level once empty.
        friend class OrderBook;
        // Test-only access to internals; defined in tests, never in the library.
        friend struct OrderBookTestAccess;

        const Price tickPrice;
        const Side side;

        OrderQueue orders;
        // Aggregate of all orders' remaining quantities. Wider than a Quantity, so it wraps only
        // past about 2^32 orders of the maximum quantity at one price.
        std::uint64_t quantity = 0;

        // Fills up to fillQuant from the oldest order only, popping it if fully filled.
        // Throws std::invalid_argument on zero, or std::runtime_error on an empty level.
        OrderFill fillOldestOrder(Quantity fillQuant);

    public:
        // A level whose queue takes its nodes from pool, or from the heap when pool is null.
        PriceLevel(Price price, Side s, NodePool* pool = nullptr);
        // A heap-backed level holding one order; for tests.
        PriceLevel(Price price, Side s, Order initialOrder);

        // Queues the order at the back, under this owner (0 for none).
        OrderLocation addOrder(Order newOrder, OwnerId owner = 0);
        // The front of the queue; the level must not be empty.
        const Order& getOldestOrder() const;

        // Erases the order and takes its remaining quantity off the aggregate.
        void removeOrder(OrderQueue::iterator itr);

        bool isEmpty() const;

        std::uint64_t getAggregateQuantity() const;
        unsigned int getOrderCount() const;
    };
}  // namespace order_book