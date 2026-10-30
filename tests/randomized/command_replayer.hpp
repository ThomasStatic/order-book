#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "command.hpp"
#include "command_generator.hpp"
#include "order_book/invariants.hpp"
#include "order_book/order_book.hpp"

namespace order_book::randomized {

// What went wrong after a command, in the order the replayer checks for it.
enum class FailureType {
    UNEXPECTED_EXCEPTION,  // the command threw, which it should do only on an engine bug or allocation failure
    INVARIANT_VIOLATION,   // OrderBook::checkInvariants found a broken invariant
    BEHAVIOR_VIOLATION,    // the engine's own result broke a behavior property, model or no model
    OUTCOME_MISMATCH,      // the result or its events differ from what the reference model predicts
    BOOK_STATE_MISMATCH,   // the book's levels, queues, or order count differ from the model's
    PROPERTY_VIOLATION     // an extra property given to the replayer failed
};

const char* failureTypeName(FailureType type);

struct Failure {
    FailureType type;
    std::size_t commandIndex;  // 0-based; the executed prefix holds commandIndex + 1 commands
    Command command;
    std::optional<InvariantViolation> violation;
    // What was expected and what happened, possibly over several lines.
    std::string reason;
    // The book and the model just after the command, around what the command touched.
    std::string bookState;

    // The same kind of failure: shrinking keeps a candidate only if it still fails this way.
    bool sameKindAs(const Failure& other) const { return type == other.type && violation == other.violation; }
};

struct RunStats {
    std::size_t commands = 0;
    std::size_t submissions = 0;
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    std::size_t trades = 0;
    std::size_t mostTradesInOneSubmission = 0;
    std::size_t expired = 0;
    std::size_t preventions = 0;
    std::size_t stopsTriggered = 0;
    std::size_t longestCascade = 0;  // stops triggered by one command
    std::size_t cancellations = 0;
    std::size_t cancelled = 0;
    std::size_t cancelsRejected = 0;
    std::size_t intentionallyInvalid = 0;
    std::size_t peakResting = 0;
    std::size_t peakLevels = 0;
    std::size_t peakPending = 0;

    std::string summary() const;
};

struct RunResult {
    std::optional<Failure> failure;
    RunStats stats;

    bool passed() const { return !failure.has_value(); }
};

// An extra check run after each command once the built-in ones pass; returns why it failed,
// or nothing. Useful for one-off properties, and for testing the harness with a planted fault.
using PropertyCheck = std::function<std::optional<std::string>(const OrderBook& book, const Command& command)>;

// Runs commands against an OrderBook beside a ReferenceBook, stopping at the first failure.
// After every command, whatever it did (accepted, rejected, traded, cancelled, refused), it
// runs the book's complete invariant check, then checks the behavior properties against the
// engine's own result and book alone, for the submitted order and each stop its trades
// triggered (so a misreading the model shares still fails: no market, IOC, FOK, or STOP order
// rests, and an unfilled one ends its events as an OrderExpired; a FOK trades all or nothing;
// a post-only order never takes; no two orders of the same nonzero owner trade, judged from
// the commands' requests; no stop pends past the last trade price), then
// compares the result and its exact events with the model's prediction, and every level, FIFO
// queue, pending stop, and the last trade price with the model's.
class CommandReplayer {
public:
    explicit CommandReplayer(PropertyCheck extraProperty = {}) : property(std::move(extraProperty)) {}

    // Runs a fixed stream. The book must be empty (throws std::invalid_argument otherwise),
    // since the model starts empty.
    RunResult replay(OrderBook& book, const std::vector<Command>& commands) const;

    // Generates and runs up to `count` commands, recording each before it runs, so after a
    // failure the recorder holds the complete executed prefix, failing command last.
    RunResult run(OrderBook& book, RandomCommandGenerator& generator, std::size_t count,
                  CommandRecorder& recorder) const;

    // Each accepted submission's request by order ID (an Order's with owner 0), from the
    // commands run, for the behavior checks.
    using RequestLedger = std::unordered_map<OrderId, OrderRequest>;

private:
    std::optional<Failure> execute(OrderBook& book, ReferenceBook& model, RequestLedger& owners, const Command& command,
                                   std::size_t index, RunStats& stats) const;

    PropertyCheck property;
};

// The book's and the model's best levels per side, and where the command's order is in each.
// Reads the book's levels directly, never through its index, so it is safe on a corrupted book.
std::string describeBookState(const OrderBook& book, const ReferenceBook& model, const Command& command);

}  // namespace order_book::randomized
