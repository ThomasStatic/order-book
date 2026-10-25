#include "order_book/replay_summary.hpp"

#include <vector>

#include "order_book/event_format.hpp"

namespace order_book::replay {
    namespace {
        // std::to_string formats integers as printf's %d family does, which no locale
        // groups or otherwise changes.
        void appendLine(std::string& text, const char* name, std::uint64_t value) {
            text += name;
            text += ": ";
            text += std::to_string(value);
            text += '\n';
        }

        void appendSide(std::string& text, const char* name, const std::vector<PriceLevelSummary>& levels) {
            text += name;
            text += ":\n";
            if (levels.empty()) {
                text += "  none\n";
                return;
            }
            for (const PriceLevelSummary& level : levels) {
                text += "  price=" + std::to_string(level.price) + " qty=" + std::to_string(level.totalQuantity)
                      + " orders=" + std::to_string(level.orderQuantity) + '\n';
            }
        }
    }  // namespace

    std::string formatReplayEvent(const ReplayEvent& event) {
        return "line " + std::to_string(event.lineNumber) + ": " + formatEvent(event.event);
    }

    std::string formatReplaySummary(const ReplayResult& result) {
        const ReplayStatistics& statistics = result.statistics;
        std::string text;
        appendLine(text, "commands processed", statistics.commandsProcessed);
        appendLine(text, "add commands", statistics.addCommands);
        appendLine(text, "adds accepted", statistics.addsAccepted);
        appendLine(text, "adds rejected", statistics.addsRejected);
        appendLine(text, "cancel commands", statistics.cancelCommands);
        appendLine(text, "cancels accepted", statistics.cancelsAccepted);
        appendLine(text, "cancels rejected", statistics.cancelsRejected);
        appendLine(text, "trades executed", statistics.tradesExecuted);
        appendLine(text, "volume traded", statistics.volumeTraded);
        appendLine(text, "resting orders", result.finalBook.totalActiveVisibleOrders);
        // The snapshot lists each side best price first, which is the order printed.
        appendSide(text, "bids", result.finalBook.bids);
        appendSide(text, "asks", result.finalBook.asks);
        return text;
    }

    std::string formatReplayError(const ReplayError& error, std::string_view path) {
        if (error.parseError) {
            return formatParseError(*error.parseError, path);
        }
        std::string text(path);
        if (error.lineNumber) {
            text += ':' + std::to_string(*error.lineNumber);
        }
        text += ": ";
        if (error.code != ReplayErrorCode::INPUT_READ_FAILURE) {
            text += "engine failure: ";
        }
        return text + error.message;
    }

}  // namespace order_book::replay
