#include "order_book/order_book.hpp"

namespace order_book {
    const Order& OrderBook::getBestBid() const{
        const PriceLevel& bestBidLevel = bids.begin()->second;
        return bestBidLevel.getOldestOrder();
    }

    const Order& OrderBook::getBestAsk() const{
        const PriceLevel& bestAskLevel = asks.begin()->second;
        return bestAskLevel.getOldestOrder();
    }

    bool OrderBook::hasBids() const {
        return !bids.empty();
    }

    bool OrderBook::hasAsks() const {
        return !asks.empty();
    }
}