#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "order_book/replay_parser.hpp"
#include "order_book/wasm/shadow_books.hpp"

// A client command as one v3 replay row, which the terminal and the sandbox both make: the row's
// columns, what a client command may leave out, and the check every replay row gets. Internal to
// order_book_wasm (docs/internals/wasm.md).
namespace order_book::wasm::rows {

    // The row's columns, in the v3 header's order.
    enum Column : std::size_t {
        SEQUENCE,
        COMMAND,
        ORDER_ID,
        SIDE,
        PRICE,
        QUANTITY,
        ORDER_TYPE,
        TIME_IN_FORCE,
        STOP_PRICE,
        POST_ONLY,
        OWNER,
        STP,
        SYMBOL,
        COLUMNS
    };

    // An ADD's order type from its price and its stop when none is given (a price makes LIMIT,
    // none MARKET, and a stop STOP_LIMIT and STOP), and its time in force, GTC with a price and
    // IOC without; for any command, CANCEL_RESTING for an owner with no mode.
    inline void fillDefaults(std::vector<std::string>& row) {
        if (row[COMMAND] == "ADD") {
            const bool priced = !row[PRICE].empty();
            const bool stop = !row[STOP_PRICE].empty();
            if (row[ORDER_TYPE].empty()) {
                row[ORDER_TYPE] = stop ? (priced ? "STOP_LIMIT" : "STOP") : (priced ? "LIMIT" : "MARKET");
            }
            if (row[TIME_IN_FORCE].empty()) {
                row[TIME_IN_FORCE] = priced ? "GTC" : "IOC";
            }
        }
        if (!row[OWNER].empty() && row[STP].empty()) {
            row[STP] = "CANCEL_RESTING";
        }
    }

    // The row checked exactly as a replay row at this sequence, whose line is the next one, or
    // what was wrong: the parser's column and message, or a symbol off the instrument list, which
    // is fixed, so it is refused before taking a line.
    inline std::variant<replay::ReplayRecord, std::string> check(std::vector<std::string> row, std::size_t sequence, const ShadowBooks& shadows) {
        row[SEQUENCE] = std::to_string(sequence);
        const replay::ParseResult parsed = replay::parseRow(replay::CsvRow{sequence + 1, std::move(row)}, replay::ReplayFormat::V3,
                                                            sequence > 1 ? std::optional<std::uint64_t>(sequence - 1) : std::nullopt);
        if (const auto* error = std::get_if<replay::ParseError>(&parsed)) {
            return (error->column.empty() ? std::string() : error->column + ": ") + error->message;
        }
        replay::ReplayRecord record = std::get<replay::ReplayRecord>(parsed);
        if (!shadows.shardOf(record.symbol)) {
            return record.symbol.text() + " is not on the instrument list";
        }
        return record;
    }

}  // namespace order_book::wasm::rows
