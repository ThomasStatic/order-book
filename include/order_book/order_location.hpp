#pragma once

#include<iterator>
#include<list>
#include "side.hpp"
#include "order.hpp"

namespace order_book {
    struct OrderLocation
    {
        unsigned int tickPrice;
        Side side;
        std::list<Order>::iterator itr;

        OrderLocation(unsigned int p, Side s, std::list<Order>::iterator i): tickPrice(p), side(s), itr(i) {}

    };
    
}