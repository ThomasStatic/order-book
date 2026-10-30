#pragma once

#include <string>
#include <string_view>

#include "replay_engine.hpp"

// Text for a replay, kept apart from the engine as event_format.hpp is from the book.
// Everything here is a pure function of its arguments: no timestamps, durations,
// addresses, or unordered iteration, and numbers are written digit by digit with no
// locale, so the same replay always prints the same bytes.
namespace order_book::replay {

    // "line <n>: " followed by formatEvent's text, e.g.
    // "line 4: TradeExecuted incoming=1003 resting=1002 price=105 qty=20".
    std::string formatReplayEvent(const ReplayEvent& event);

    // The statistics in a fixed order (each command kind's count beside its accepted and
    // rejected counts, then trades, volume, and the other outcomes), ending with the orders
    // resting and the stops pending in the final book: one "name: value" line each, every
    // line ending with '\n'. Every line is always printed. For example:
    //
    //   commands processed: 4
    //   add commands: 3
    //   adds accepted: 3
    //   adds rejected: 0
    //   cancel commands: 1
    //   cancels accepted: 1
    //   cancels rejected: 0
    //   replace commands: 0
    //   replaces accepted: 0
    //   replaces rejected: 0
    //   trades executed: 1
    //   volume traded: 20
    //   self-trades prevented: 0
    //   orders expired: 0
    //   stops triggered: 0
    //   resting orders: 1
    //   pending stops: 0
    std::string formatReplayStatistics(const ReplayResult& result);

    // Each side's levels best price first (bids descending, asks ascending), one
    // "  price=<ticks> qty=<total> orders=<count>" line per level, or "  none" for an empty
    // side. Every line ends with '\n'. For example:
    //
    //   bids:
    //     none
    //   asks:
    //     price=105 qty=10 orders=1
    std::string formatFinalBook(const BookSnapshot& book);

    // The replay's final book as above, then "last trade price: <ticks>" (or "none" before any
    // trade). Pending stops are not listed.
    std::string formatFinalBook(const ReplayResult& result);

    // formatReplayStatistics, then formatFinalBook of the result.
    std::string formatReplaySummary(const ReplayResult& result);

    // One diagnostic line for path. A parse error prints as formatParseError does
    // ("<path>:<line>: <column>: <message>"). Other errors print as "<path>:<line>: <message>",
    // or "<path>: <message>" with no line, and an engine failure (an exception or a broken
    // invariant) is marked "engine failure: " to set it apart from bad input.
    std::string formatReplayError(const ReplayError& error, std::string_view path);

}  // namespace order_book::replay
