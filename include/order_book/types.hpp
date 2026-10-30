#pragma once

namespace order_book {
    // Prices are integer ticks of a fixed $0.01. Zero is invalid for every one of these.
    using Price = unsigned int;
    using Quantity = unsigned int;
    using OrderId = unsigned int;
    using TradeId = unsigned int;
    using SequenceNum = unsigned int;

    // Who an order belongs to, for self-trade prevention (FIX SelfMatchPreventionID(2362)): an
    // opaque ID the book compares and never interprets or validates. The one alias where 0 is
    // valid: it means no owner, and an order without one is never prevented.
    using OwnerId = unsigned int;
}  // namespace order_book
