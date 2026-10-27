#pragma once

#include <list>

#include "node_pool.hpp"
#include "order.hpp"
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
}  // namespace order_book