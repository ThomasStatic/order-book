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
        void collectEvents(std::span<const BookEvent> events, std::size_t lineNumber,
                           std::vector<ReplayEvent>& replayEvents) {
            for (const BookEvent& event : events) {
                replayEvents.push_back(ReplayEvent{lineNumber, event});
            }
        }

        void runAdd(const AddCommand& add, std::size_t lineNumber, OrderBook& book, ReplayResult& result) {
            // The book assigns the real sequence number on acceptance; 1 is a placeholder.
            const SubmissionResults submission = book.submitOrder(Order(add.orderId, add.price, add.quantity, 1, add.side));

            ReplayStatistics& statistics = result.statistics;
            ++statistics.addCommands;
            if (submission.getStatus() == SubmissionStatus::ACCEPTED) {
                ++statistics.addsAccepted;
            } else {
                ++statistics.addsRejected;
            }
            for (const Trade& trade : submission.getGeneratedTrades()) {
                ++statistics.tradesExecuted;
                statistics.volumeTraded += trade.getExecutionQuantity();
            }
            collectEvents(submission.getEvents(), lineNumber, result.events);
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
            collectEvents(cancellation.getEvents(), lineNumber, result.events);
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
        for (const ReplayRecord& record : records) {
            try {
                std::visit([&](const auto& command) {
                    using Kind = std::decay_t<decltype(command)>;
                    if constexpr (std::is_same_v<Kind, AddCommand>) {
                        runAdd(command, record.lineNumber, book, result);
                    } else {
                        static_assert(std::is_same_v<Kind, CancelCommand>, "every Command alternative needs a dispatch");
                        runCancel(command, record.lineNumber, book, result);
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
