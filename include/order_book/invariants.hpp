#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "side.hpp"
#include "types.hpp"

// Whether every public command that can change an OrderBook checks the whole book before
// returning (or throwing). Unless defined, it is on exactly when NDEBUG is not, so in Debug
// but not Release builds; the ORDER_BOOK_INVARIANT_CHECKS CMake setting (AUTO, ON, or OFF)
// defines it as 1 or 0 for every configuration. OrderBook::checkInvariants stays callable
// either way.
#ifndef ORDER_BOOK_INVARIANT_CHECKS
#  ifdef NDEBUG
#    define ORDER_BOOK_INVARIANT_CHECKS 0
#  else
#    define ORDER_BOOK_INVARIANT_CHECKS 1
#  endif
#endif

namespace order_book {
    inline constexpr bool invariantChecksAfterCommands = ORDER_BOOK_INVARIANT_CHECKS != 0;

    // A promise the book's own structures broke: a bug in the engine, not a bad request.
    // Listed roughly in the order checkInvariants looks for them: per level and order on
    // bids then asks, then against the index, then across the top of the book.
    enum class InvariantViolation {
        INVALID_BID_ORDERING,         // bid levels not strictly descending by price
        INVALID_ASK_ORDERING,         // ask levels not strictly ascending by price
        MISPLACED_LEVEL,              // a level's own price or side differs from its slot in the book
        EMPTY_PRICE_LEVEL,
        ZERO_QUANTITY_ORDER,          // a resting order with nothing left to fill
        MALFORMED_ORDER,              // a field Order refuses, a zero sequence number, or marked FILLED
        MISPLACED_ORDER,              // an order's price or side differs from its level's
        INVALID_FIFO_SEQUENCE,        // a level's sequence numbers out of acceptance order
        AGGREGATE_QUANTITY_MISMATCH,  // a level's cached quantity is not its orders' remaining sum
        MISSING_INDEX_ENTRY,          // a resting order with no index entry
        DUPLICATE_ORDER_ID,           // two resting orders, on either side, with one ID
        INVALID_INDEX_ENTRY,          // an entry recording the wrong place, or no resting order
        CROSSED_BOOK                  // best bid at or above best ask
    };

    // The enumerator's name, for printing violations as identifiers.
    constexpr const char* invariantViolationName(InvariantViolation violation) {
        switch(violation) {
            case InvariantViolation::INVALID_BID_ORDERING: return "INVALID_BID_ORDERING";
            case InvariantViolation::INVALID_ASK_ORDERING: return "INVALID_ASK_ORDERING";
            case InvariantViolation::MISPLACED_LEVEL: return "MISPLACED_LEVEL";
            case InvariantViolation::EMPTY_PRICE_LEVEL: return "EMPTY_PRICE_LEVEL";
            case InvariantViolation::ZERO_QUANTITY_ORDER: return "ZERO_QUANTITY_ORDER";
            case InvariantViolation::MALFORMED_ORDER: return "MALFORMED_ORDER";
            case InvariantViolation::MISPLACED_ORDER: return "MISPLACED_ORDER";
            case InvariantViolation::INVALID_FIFO_SEQUENCE: return "INVALID_FIFO_SEQUENCE";
            case InvariantViolation::AGGREGATE_QUANTITY_MISMATCH: return "AGGREGATE_QUANTITY_MISMATCH";
            case InvariantViolation::MISSING_INDEX_ENTRY: return "MISSING_INDEX_ENTRY";
            case InvariantViolation::DUPLICATE_ORDER_ID: return "DUPLICATE_ORDER_ID";
            case InvariantViolation::INVALID_INDEX_ENTRY: return "INVALID_INDEX_ENTRY";
            case InvariantViolation::CROSSED_BOOK: return "CROSSED_BOOK";
        }
        return "UNKNOWN";
    }

    // What OrderBook::checkInvariants found: no violation if the book is sound, otherwise
    // the first one and where it is. Fields that do not apply to a violation stay empty.
    struct InvariantResult {
        std::optional<InvariantViolation> violation{};
        // The side and price of the offending level (CROSSED_BOOK has neither), and the
        // offending order's ID or index key.
        std::optional<Side> side{};
        std::optional<Price> price{};
        std::optional<OrderId> orderId{};
        // The value the invariant requires and the one found, where a number tells them apart:
        // the previous level's price and this one's for the orderings; the slot's and the
        // level's price for MISPLACED_LEVEL; the level's and the order's price for
        // MISPLACED_ORDER; the sequence number of the order ahead and this order's for
        // INVALID_FIFO_SEQUENCE; the summed and the cached quantity for
        // AGGREGATE_QUANTITY_MISMATCH; the level's and the recorded price, or the resting
        // order count and the index size, for INVALID_INDEX_ENTRY; the best ask and the best
        // bid for CROSSED_BOOK.
        std::optional<std::uint64_t> expected{};
        std::optional<std::uint64_t> actual{};
        // Which check failed, in words. A string literal, so building a result never allocates.
        const char* detail = "";

        bool isValid() const { return !violation.has_value(); }
    };

    // One line, e.g. "AGGREGATE_QUANTITY_MISMATCH side=BUY price=100 expected=8 actual=9
    // (a level's cached quantity should equal ...)", or "valid".
    std::string formatInvariantResult(const InvariantResult& result);
}  // namespace order_book
