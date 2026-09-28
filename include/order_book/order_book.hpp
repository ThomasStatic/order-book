#pragma once

#include <unordered_map>
#include <map>
#include <compare>
#include <stdexcept>
#include <vector>

#include "order_location.hpp"
#include "price_level.hpp"
#include "order.hpp"

namespace order_book {
    
    using Price = unsigned int;
    using OrderId = unsigned int;

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
        std::unordered_map<OrderId, OrderLocation> orderIndex;

        std::map<Price, PriceLevel, std::greater<Price>> bids;
        std::map<Price, PriceLevel, std::less<Price>> asks;

    public:
        OrderBook() = default;

        const Order& getBestBid() const;
        const Order& getBestAsk() const;

        bool hasBids() const;
        bool hasAsks() const;

        void addOrder(Order order);

        OrderLocation findActiveOrder(OrderId orderId) const;

        void removeOrder(OrderId orderId);

        // Fills resting orders at one price level in FIFO order, unindexing fully
        // filled orders and erasing the level once it is empty.
        ConsumptionResult consumeLevel(Side side, Price price, unsigned int quantity);

        BookSnapshot snapshot(unsigned int depth = 5) const;

    };
    
}  // namespace order_book
