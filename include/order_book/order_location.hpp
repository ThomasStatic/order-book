#pragma once

#include <list>

#include "node_pool.hpp"
#include "order.hpp"
#include "order_request.hpp"
#include "side.hpp"
#include "types.hpp"

namespace order_book {
    class PriceLevel;

    // A price level's FIFO queue of resting orders. Its nodes come from the owning book's
    // NodePool, so resting and removing orders reuse memory instead of going to the heap.
    using OrderQueue = std::list<Order, PoolAllocator<Order>>;

    // Where a resting order is: what OrderBook's index records for each order ID.
    struct OrderLocation
    {
        Price tickPrice;
        Side side;
        OrderQueue::iterator itr;
        // The level the order is queued in, so cancelOrder reaches it without searching the
        // side's map. Valid while the order rests: a level is erased only once empty, after
        // every order in it has been unindexed, and std::map never moves its elements.
        PriceLevel* level;

        OrderLocation(Price p, Side s, OrderQueue::iterator i, PriceLevel* l)
            : tickPrice(p), side(s), itr(i), level(l) {}
    };

    // A stop waiting for its trigger: the request as accepted, every field kept until it
    // triggers, and the sequence number consumed on acceptance, which orders stops at one stop
    // price.
    struct PendingStop {
        OrderRequest request;
        SequenceNum sequenceNum;
        Price triggeredAt = 0;  // the trade price that triggered it, once in the triggered queue
    };

    // A stop price's FIFO queue of pending stops, its nodes from the book's NodePool.
    using StopQueue = std::list<PendingStop, PoolAllocator<PendingStop>>;

    // The pending stops at one stop price on one side, oldest first.
    struct StopLevel {
        Price stopPrice;
        Side side;
        StopQueue stops;

        StopLevel(Price price, Side levelSide, NodePool* pool)
            : stopPrice(price), side(levelSide), stops(PoolAllocator<PendingStop>(pool)) {}
    };

    // Where a pending stop is, as the pending-stop index records it: its stop price (named as
    // a resting order's price is, so one index serves both), side, queue position, and level.
    struct StopLocation {
        Price tickPrice;
        Side side;
        StopQueue::iterator itr;
        StopLevel* level;

        StopLocation(Price p, Side s, StopQueue::iterator i, StopLevel* l) : tickPrice(p), side(s), itr(i), level(l) {}
    };
}  // namespace order_book