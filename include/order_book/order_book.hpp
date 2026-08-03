#pragma once

#include <unordered_map>
#include <map>
#include <compare>

#include "order_location.hpp"
#include "price_level.hpp"
#include "order.hpp"

namespace order_book {
    
    using Price = unsigned int;
    using OrderId = unsigned int;

    class OrderBook final {
    private:
        std::unordered_map<OrderId, OrderLocation> orderIndex;

        std::map<Price, PriceLevel, std::greater<Price>> bids;
        std::map<Price, PriceLevel, std::less<Price>> asks;

        const Order& getBestBid() const;
        const Order& getBestAsk() const;

        bool hasBids() const;
        bool hasAsks() const;
        
        void addOrder(Order order);

    public:
        OrderBook() = default;
    };
    
}  // namespace order_book
