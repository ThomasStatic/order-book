#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "book_event.hpp"
#include "order_book.hpp"
#include "replay_parser.hpp"

// The replay engine (docs/replay-format.md): it hands parsed commands to the book in file
// order and collects what the book did. It implements no trading rules. readReplayFile and
// readReplayInput gather records through replay_parser.hpp's ReplayReader, which does all
// the parsing; ReplayEngine::run sees only typed records. Printing is replay_summary.hpp's.
// Built into order_book_replay with the parser.
namespace order_book::replay {

    // One lifecycle event, as the book returned it, with the physical line of the command
    // that produced it.
    struct ReplayEvent {
        std::size_t lineNumber;
        BookEvent event;

        bool operator==(const ReplayEvent&) const = default;
    };

    // What happened, without reading every event. Command counts first, then executions.
    // commandsProcessed == addCommands + cancelCommands, addCommands == addsAccepted +
    // addsRejected, and cancelCommands == cancelsAccepted + cancelsRejected.
    struct ReplayStatistics {
        std::size_t commandsProcessed = 0;

        std::size_t addCommands = 0;
        std::size_t cancelCommands = 0;

        std::size_t addsAccepted = 0;
        std::size_t addsRejected = 0;

        std::size_t cancelsAccepted = 0;
        std::size_t cancelsRejected = 0;

        std::size_t tradesExecuted = 0;
        std::uint64_t volumeTraded = 0;  // the sum of every trade's quantity, which can exceed one Quantity

        // v2's commands and outcomes, appended so v1's initializers keep their meaning;
        // processed = adds + cancels + replaces. Counted from events, so the engine implements
        // no trading rules; an ADD accepted then expired still counts as accepted.
        std::size_t replaceCommands = 0;
        std::size_t replacesAccepted = 0;
        std::size_t replacesRejected = 0;
        std::size_t selfTradesPrevented = 0;
        std::size_t ordersExpired = 0;
        std::size_t stopsTriggered = 0;

        bool operator==(const ReplayStatistics&) const = default;
    };

    struct ReplayResult {
        std::vector<ReplayEvent> events;  // in the order the book produced them
        ReplayStatistics statistics;
        BookSnapshot finalBook;           // every level of both sides, best price first
        std::size_t pendingStops = 0;     // the book's pendingStopCount at the end
        std::optional<Price> lastTradePrice;  // the book's at the end; none before any trade
    };

    enum class ReplayErrorCode {
        INPUT_READ_FAILURE,          // the input could not be opened or read, or failed to parse
        INTERNAL_EXECUTION_FAILURE,  // the book threw while running a command
        INVARIANT_VIOLATION          // the book failed checkInvariants
    };

    // The enumerator's name, for printing codes as identifiers.
    constexpr const char* replayErrorCodeName(ReplayErrorCode code) {
        switch(code) {
            case ReplayErrorCode::INPUT_READ_FAILURE: return "INPUT_READ_FAILURE";
            case ReplayErrorCode::INTERNAL_EXECUTION_FAILURE: return "INTERNAL_EXECUTION_FAILURE";
            case ReplayErrorCode::INVARIANT_VIOLATION: return "INVARIANT_VIOLATION";
        }
        return "UNKNOWN";
    }

    // A fatal error: the replay stops there and nothing after it runs. Commands that already
    // ran are not rolled back.
    struct ReplayError {
        ReplayErrorCode code;
        // The physical line at fault, when one is: the bad record, the command that threw, or
        // the command after which a per-command check failed. Empty for a file that can't be
        // opened or read, and for a failed final check.
        std::optional<std::size_t> lineNumber;
        std::string message;
        // The parser's diagnostic, when the input was malformed.
        std::optional<ParseError> parseError;
    };

    // How often run checks the whole book with checkInvariants, independent of the book's
    // own automatic checks (invariantChecksAfterCommands), which abort rather than report.
    // A check is O(N + L) in resting orders and levels, so PER_COMMAND makes a replay
    // quadratic. FINAL is cheap but cannot say which command broke the book; replaying the
    // same input on a fresh book with PER_COMMAND finds the first line after which it fails.
    enum class InvariantValidation {
        NONE,
        FINAL,        // once, after the last command
        PER_COMMAND   // after every command, so the last check is also the final one
    };

    struct ReplayOptions {
        InvariantValidation invariantValidation = InvariantValidation::FINAL;
    };

    using ReplayOutcome = std::variant<ReplayResult, ReplayError>;

    class ReplayEngine {
    public:
        // Runs each record against book, strictly in the order given, collecting each
        // command's events unchanged. ADD goes to submitOrder (never addOrder) and CANCEL to
        // cancelOrder, so a command the book refuses is a rejection event, and the replay
        // carries on. Stops at the first exception from the book
        // (INTERNAL_EXECUTION_FAILURE) or failed invariant check (INVARIANT_VIOLATION),
        // leaving the book as that command left it. The same book state, records, and
        // options always give the same outcome; a replay of a file starts from an empty book.
        static ReplayOutcome run(const std::vector<ReplayRecord>& records, OrderBook& book,
                                 const ReplayOptions& options = {});
    };

    // Every record the input yields, up to its first error, which is then held in error.
    // The format fails fast, so the records before the error are the ones a replay runs.
    struct ReplayInput {
        std::vector<ReplayRecord> records;
        std::optional<ReplayError> error;  // always INPUT_READ_FAILURE
    };

    // Reads through a ReplayReader; a stream that fails to read is an INPUT_READ_FAILURE
    // with no line, after the records completed before the failure. Open files in binary
    // mode (see ReplayReader). The reader reads ahead in chunks, so the stream's position
    // afterwards is not just past the last record.
    ReplayInput readReplayInput(std::istream& input);

    // Opens path in binary mode and reads it; a file that can't be opened is an
    // INPUT_READ_FAILURE with no line and no records.
    ReplayInput readReplayFile(const std::filesystem::path& path);

}  // namespace order_book::replay
