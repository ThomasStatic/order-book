#pragma once

#include <string>
#include <variant>
#include <vector>

#include "replay_engine.hpp"
#include "replay_parser.hpp"
#include "symbol.hpp"

// Replaying input that names instruments: the records are grouped by symbol, and the unchanged
// ReplayEngine runs once per book. There is no thread and nothing shared, and nothing orders
// one instrument against another, so each instrument's output stands alone. A book exists for
// each symbol in a record, so no instrument list is needed.
namespace order_book::replay {

    // One instrument's records, in their file order.
    struct InstrumentRecords {
        Symbol symbol;
        std::vector<ReplayRecord> records;
    };

    // One entry per symbol that appears, sorted by symbol bytes. A record with no symbol takes
    // the one given, for a v1 or v2 file standing in for one instrument.
    std::vector<InstrumentRecords> groupBySymbol(const std::vector<ReplayRecord>& records, Symbol standIn = Symbol{});

    struct InstrumentReplay {
        Symbol symbol;
        ReplayResult result;
    };

    // The book that failed, and why. Books run in symbol order, so later ones did not run.
    struct InstrumentFailure {
        Symbol symbol;
        ReplayError error;
        // Where a failed final check first went wrong, from replaying that book alone with
        // per-command checks; empty if that was not done or found nothing.
        std::vector<ReplayError> located;
    };

    using InstrumentsOutcome = std::variant<std::vector<InstrumentReplay>, InstrumentFailure>;

    InstrumentsOutcome replayInstruments(const std::vector<InstrumentRecords>& instruments, const ReplayOptions& options);

    // What one book's output holds.
    struct ReplayOutput {
        bool trades = false;
        bool events = false;  // every event, which covers trades
        bool book = false;
        bool statistics = true;
    };

    // One book's output: the trades or events asked for, a blank line if statistics follow any,
    // the statistics, then the final book.
    std::string formatReplayOutput(const ReplayResult& result, const ReplayOutput& output);

    // One section per instrument, each "symbol <text>" then formatReplayOutput, with a blank
    // line between sections.
    std::string formatInstrumentSections(const std::vector<InstrumentReplay>& instruments, const ReplayOutput& output);

}  // namespace order_book::replay
