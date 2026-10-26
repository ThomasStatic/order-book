#pragma once

#include<iterator>
#include<list>
#include "side.hpp"
#include "order.hpp"

namespace order_book {
    class PriceLevel;

    struct OrderLocation
    {
        unsigned int tickPrice;
        Side side;
        std::list<Order>::iterator itr;
        // The level the order is queued in, so cancelOrder reaches it without searching the
        // side's map. Valid while the order rests: a level is erased only once empty, after
        // every order in it has been unindexed, and std::map never moves its elements.
        PriceLevel* level;

        OrderLocation(unsigned int p, Side s, std::list<Order>::iterator i, PriceLevel* l)
            : tickPrice(p), side(s), itr(i), level(l) {}

    };
    
}