#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include "invariants.hpp"

// Why OrderBook::restore refused a book image. The image's bytes are specified in
// docs/checkpoint-format.md, which lists every code with the offset it reports.
namespace order_book {

    // An open set: codes are only ever appended, never reordered.
    enum class RestoreErrorCode {
        TOO_SHORT,                // fewer bytes than a header, or than the stated header bytes
        UNSUPPORTED_VERSION,      // an image version this reader cannot read
        SIZE_BELOW_KNOWN,         // a stated header or record size below this version's
        LENGTH_MISMATCH,          // a length other than the counts give, trailing bytes included
        ZERO_FIELD,               // a zero next sequence number or trade ID, or a zero order or stop ID
        NONZERO_RESERVED,         // reserved bytes that are not 0
        INDEX_SIZE_MISMATCH,      // an index size other than its record count
        EMPTY_LEVEL,              // a level with no orders
        ORDER_COUNT_MISMATCH,     // level order counts that disagree with the orders written
        LEVELS_OUT_OF_ORDER,      // levels not strictly best first
        AGGREGATE_MISMATCH,       // a level aggregate other than its orders' remaining sum
        INVALID_ENUMERATOR,       // a byte outside its field's values, a FILLED order among them
        REMAINING_ABOVE_INITIAL,  // an order with more remaining than its initial quantity
        STOPS_OUT_OF_ORDER,       // stops not buys by ascending, then sells by descending, stop price
        DUPLICATE_ID,             // an ID resting twice, pending twice, or both
        INVARIANT_VIOLATION       // the rebuilt book fails checkInvariants
    };

    // The enumerator's name, for printing codes as identifiers.
    constexpr const char* restoreErrorCodeName(RestoreErrorCode code) {
        switch(code) {
            case RestoreErrorCode::TOO_SHORT: return "TOO_SHORT";
            case RestoreErrorCode::UNSUPPORTED_VERSION: return "UNSUPPORTED_VERSION";
            case RestoreErrorCode::SIZE_BELOW_KNOWN: return "SIZE_BELOW_KNOWN";
            case RestoreErrorCode::LENGTH_MISMATCH: return "LENGTH_MISMATCH";
            case RestoreErrorCode::ZERO_FIELD: return "ZERO_FIELD";
            case RestoreErrorCode::NONZERO_RESERVED: return "NONZERO_RESERVED";
            case RestoreErrorCode::INDEX_SIZE_MISMATCH: return "INDEX_SIZE_MISMATCH";
            case RestoreErrorCode::EMPTY_LEVEL: return "EMPTY_LEVEL";
            case RestoreErrorCode::ORDER_COUNT_MISMATCH: return "ORDER_COUNT_MISMATCH";
            case RestoreErrorCode::LEVELS_OUT_OF_ORDER: return "LEVELS_OUT_OF_ORDER";
            case RestoreErrorCode::AGGREGATE_MISMATCH: return "AGGREGATE_MISMATCH";
            case RestoreErrorCode::INVALID_ENUMERATOR: return "INVALID_ENUMERATOR";
            case RestoreErrorCode::REMAINING_ABOVE_INITIAL: return "REMAINING_ABOVE_INITIAL";
            case RestoreErrorCode::STOPS_OUT_OF_ORDER: return "STOPS_OUT_OF_ORDER";
            case RestoreErrorCode::DUPLICATE_ID: return "DUPLICATE_ID";
            case RestoreErrorCode::INVARIANT_VIOLATION: return "INVARIANT_VIOLATION";
        }
        return "UNKNOWN";
    }

    // A bad image, as a value: the caller knows what it means (a live book broken, or storage
    // gone bad), so restore never aborts.
    struct RestoreError {
        RestoreErrorCode code;
        // The first byte of the field at fault, or the image's length when it is the length that
        // is wrong; 0 for INVARIANT_VIOLATION, whose result names the order instead.
        std::size_t offset = 0;
        std::string message;
        // What checkInvariants found, for INVARIANT_VIOLATION only.
        std::optional<InvariantResult> invariant{};
    };

}  // namespace order_book
