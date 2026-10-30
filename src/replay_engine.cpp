#include "order_book/replay_engine.hpp"

#include <exception>
#include <fstream>
#include <istream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "order_book/invariants.hpp"
#include "order_book/order.hpp"

namespace order_book::replay {
    namespace {
        // Every event a command produced, its cascade's included, tagged with the command's
        // line, and the statistics that count events.
        void collectEvents(std::span<const BookEvent> events, std::size_t lineNumber, ReplayResult& result) {
            ReplayStatistics& statistics = result.statistics;
            for (const BookEvent& event : events) {
                if (const auto* trade = std::get_if<TradeExecuted>(&event)) {
                    ++statistics.tradesExecuted;
                    statistics.volumeTraded += trade->executionQuantity;
                } else if (std::holds_alternative<SelfTradePrevented>(event)) {
                    ++statistics.selfTradesPrevented;
                } else if (std::holds_alternative<OrderExpired>(event)) {
                    ++statistics.ordersExpired;
                } else if (std::holds_alternative<StopTriggered>(event)) {
                    ++statistics.stopsTriggered;
                }
                result.events.push_back(ReplayEvent{lineNumber, event});
            }
        }

        void runAdd(const AddCommand& add, std::size_t lineNumber, OrderBook& book, ReplayResult& result) {
            // Always the request: for v1's fields it is exactly submitOrder(Order).
            const SubmissionResult submission =
                book.submitOrder(OrderRequest{add.orderId, add.side, add.price, add.quantity, add.orderType, add.timeInForce,
                                              add.stopPrice, add.postOnly, add.owner, add.selfTradePrevention});

            ReplayStatistics& statistics = result.statistics;
            ++statistics.addCommands;
            if (submission.getStatus() == SubmissionStatus::ACCEPTED) {
                ++statistics.addsAccepted;
            } else {
                ++statistics.addsRejected;
            }
            collectEvents(submission.getEvents(), lineNumber, result);
        }

        void runReplace(const ReplaceCommand& replace, std::size_t lineNumber, OrderBook& book, ReplayResult& result) {
            const ReplaceResult replacement = book.replaceOrder(ReplaceRequest{
                replace.orderId, replace.price, replace.quantity, replace.postOnly, replace.owner, replace.selfTradePrevention});

            ReplayStatistics& statistics = result.statistics;
            ++statistics.replaceCommands;
            if (replacement.isSuccess()) {
                ++statistics.replacesAccepted;
            } else {
                ++statistics.replacesRejected;
            }
            collectEvents(replacement.getEvents(), lineNumber, result);
        }

        void runCancel(const CancelCommand& cancel, std::size_t lineNumber, OrderBook& book, ReplayResult& result) {
            const CancellationResult cancellation = book.cancelOrder(cancel.orderId);

            ReplayStatistics& statistics = result.statistics;
            ++statistics.cancelCommands;
            if (cancellation.isSuccess()) {
                ++statistics.cancelsAccepted;
            } else {
                ++statistics.cancelsRejected;
            }
            collectEvents(cancellation.getEvents(), lineNumber, result);
        }

        // An INVARIANT_VIOLATION if the book is broken; when names the point checked.
        std::optional<ReplayError> checkBook(const OrderBook& book, std::optional<std::size_t> lineNumber,
                                             const char* when) {
            const InvariantResult check = book.checkInvariants();
            if (check.isValid()) {
                return std::nullopt;
            }
            return ReplayError{ReplayErrorCode::INVARIANT_VIOLATION, lineNumber,
                               std::string("invariant violated after ") + when + ": " + formatInvariantResult(check),
                               std::nullopt};
        }
    }  // namespace

    ReplayOutcome ReplayEngine::run(const std::vector<ReplayRecord>& records, OrderBook& book,
                                    const ReplayOptions& options) {
        const InvariantValidation validation = options.invariantValidation;
        ReplayResult result;
        // Every command makes at least one event, so this much is certain to be used, and the
        // vector does not copy its events through every doubling up to it.
        result.events.reserve(records.size());
        for (const ReplayRecord& record : records) {
            try {
                std::visit([&](const auto& command) {
                    using Kind = std::decay_t<decltype(command)>;
                    if constexpr (std::is_same_v<Kind, AddCommand>) {
                        runAdd(command, record.lineNumber, book, result);
                    } else if constexpr (std::is_same_v<Kind, CancelCommand>) {
                        runCancel(command, record.lineNumber, book, result);
                    } else {
                        static_assert(std::is_same_v<Kind, ReplaceCommand>, "every Command alternative needs a dispatch");
                        runReplace(command, record.lineNumber, book, result);
                    }
                }, record.command);
            } catch (const std::exception& e) {
                return ReplayError{ReplayErrorCode::INTERNAL_EXECUTION_FAILURE, record.lineNumber,
                                   std::string("the book threw: ") + e.what(), std::nullopt};
            }
            ++result.statistics.commandsProcessed;

            if (validation == InvariantValidation::PER_COMMAND) {
                if (std::optional<ReplayError> error = checkBook(book, record.lineNumber, "this command")) {
                    return std::move(*error);
                }
            }
        }

        // PER_COMMAND has already checked the final book, unless no command ran.
        if (validation == InvariantValidation::FINAL
            || (validation == InvariantValidation::PER_COMMAND && records.empty())) {
            if (std::optional<ReplayError> error = checkBook(book, std::nullopt, "the last command")) {
                return std::move(*error);
            }
        }

        result.finalBook = book.snapshot(std::numeric_limits<unsigned int>::max());
        result.pendingStops = book.pendingStopCount();
        result.lastTradePrice = book.getLastTradePrice();
        return result;
    }

    ReplayInput readReplayInput(std::istream& input) {
        ReplayInput replayInput;
        ReplayReader reader(input);
        try {
            while (true) {
                ReadResult result = reader.next();
                if (ReplayRecord* record = std::get_if<ReplayRecord>(&result)) {
                    replayInput.records.push_back(std::move(*record));
                } else if (ParseError* error = std::get_if<ParseError>(&result)) {
                    const std::size_t lineNumber = error->lineNumber;
                    std::string message = error->message;
                    replayInput.error = ReplayError{ReplayErrorCode::INPUT_READ_FAILURE, lineNumber,
                                                    std::move(message), std::move(*error)};
                    return replayInput;
                } else {
                    return replayInput;
                }
            }
        } catch (const std::runtime_error& e) {
            // The stream itself failed; the records read before it still stand.
            replayInput.error = ReplayError{ReplayErrorCode::INPUT_READ_FAILURE, std::nullopt, e.what(), std::nullopt};
            return replayInput;
        }
    }

    ReplayInput readReplayFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            ReplayInput replayInput;
            replayInput.error = ReplayError{ReplayErrorCode::INPUT_READ_FAILURE, std::nullopt,
                                            "cannot open replay file", std::nullopt};
            return replayInput;
        }
        return readReplayInput(file);
    }

}  // namespace order_book::replay
