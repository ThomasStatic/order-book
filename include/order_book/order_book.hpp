#pragma once

#include<unordered_map>

#include "order_location.hpp"

namespace order_book {
    
    class OrderBook final {
    public:
        OrderBook() = default;
        std::unordered_map<unsigned int, OrderLocation> orderIndex;
    };
    
}  // namespace order_book
