#include "order_book/replay_instruments.hpp"

#include <algorithm>
#include <map>
#include <utility>

#include "order_book/order_book.hpp"
#include "order_book/replay_summary.hpp"

namespace order_book::replay {

    std::vector<InstrumentRecords> groupBySymbol(const std::vector<ReplayRecord>& records, Symbol standIn) {
        std::map<Symbol, std::vector<ReplayRecord>> grouped;
        for (const ReplayRecord& record : records) {
            grouped[record.symbol.empty() ? standIn : record.symbol].push_back(record);
        }
        std::vector<InstrumentRecords> instruments;
        instruments.reserve(grouped.size());
        for (auto& [symbol, group] : grouped) {
            instruments.push_back(InstrumentRecords{symbol, std::move(group)});
        }
        return instruments;
    }

    InstrumentsOutcome replayInstruments(const std::vector<InstrumentRecords>& instruments, const ReplayOptions& options) {
        std::vector<InstrumentReplay> replays;
        replays.reserve(instruments.size());
        for (const InstrumentRecords& instrument : instruments) {
            OrderBook book;
            ReplayOutcome outcome = ReplayEngine::run(instrument.records, book, options);
            if (ReplayError* failure = std::get_if<ReplayError>(&outcome)) {
                InstrumentFailure failed{instrument.symbol, *failure, {}};
                if (failure->code == ReplayErrorCode::INVARIANT_VIOLATION && !failure->lineNumber) {
                    ReplayOptions perCommand = options;
                    perCommand.invariantValidation = InvariantValidation::PER_COMMAND;
                    OrderBook freshBook;
                    const ReplayOutcome rerun = ReplayEngine::run(instrument.records, freshBook, perCommand);
                    if (const ReplayError* located = std::get_if<ReplayError>(&rerun)) {
                        failed.located.push_back(*located);
                    }
                }
                return failed;
            }
            replays.push_back(InstrumentReplay{instrument.symbol, std::get<ReplayResult>(std::move(outcome))});
        }
        return replays;
    }

    std::string formatReplayOutput(const ReplayResult& result, const ReplayOutput& output) {
        std::string text;
        for (const ReplayEvent& event : result.events) {
            const bool isTrade = std::holds_alternative<TradeExecuted>(event.event);
            if (output.events || (output.trades && isTrade)) {
                text += formatReplayEvent(event) + '\n';
            }
        }
        if (output.statistics) {
            if (!text.empty()) {
                text += '\n';
            }
            text += formatReplayStatistics(result);
        }
        if (output.book) {
            text += formatFinalBook(result);
        }
        return text;
    }

    std::string formatInstrumentSections(const std::vector<InstrumentReplay>& instruments, const ReplayOutput& output) {
        std::string text;
        for (const InstrumentReplay& instrument : instruments) {
            if (!text.empty()) {
                text += '\n';
            }
            text += "symbol " + instrument.symbol.text() + '\n' + formatReplayOutput(instrument.result, output);
        }
        return text;
    }

}  // namespace order_book::replay
