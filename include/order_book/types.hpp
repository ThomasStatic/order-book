#pragma once

namespace order_book {
    // Prices are integer ticks of a fixed $0.01. Zero is invalid for every one of these.
    using Price = unsigned int;
    using Quantity = unsigned int;
    using OrderId = unsigned int;
    using TradeId = unsigned int;
    using SequenceNum = unsigned int;
}  // namespace order_book
