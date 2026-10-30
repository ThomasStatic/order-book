#include "command_replayer.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <list>
#include <sstream>
#include <stdexcept>
#include <variant>

#include "abort_reporter.hpp"
#include "order_book/event_format.hpp"

namespace order_book {

// Befriended by OrderBook, PriceLevel, and Order, so the harness can build the malformed
// orders the Order constructor refuses, and read levels and queues without going through the
// index. This is its only definition in the randomized harness.
struct OrderBookTestAccess {
    static Order build(const randomized::Command& command) {
        Order order(1, 1, 1, 1, Side::BUY);
        order.orderId = command.orderId;
        order.priceTick = command.price;
        order.initialQuantity = command.quantity;
        order.remainingQuantity = command.quantity;
        order.side = command.side;
        order.type = command.orderType;
        if (command.markedFilled) {
            order.fillStatus = FillStatus::FILLED;
        }
        return order;
    }

    static const auto& bids(const OrderBook& book) { return book.bids; }
    static const auto& asks(const OrderBook& book) { return book.asks; }
    static const order_book::OrderQueue& orders(const PriceLevel& level) { return level.orders; }
};

}  // namespace order_book

namespace order_book::randomized {

namespace {

using Access = OrderBookTestAccess;

// How many levels per side, and orders per level, a failure report shows.
constexpr std::size_t shownLevels = 5;
constexpr std::size_t shownOrders = 6;

const char* sideName(Side side) {
    return side == Side::BUY ? "BUY" : side == Side::SELL ? "SELL" : "INVALID";
}

const char* fillStatusName(FillStatus status) {
    switch (status) {
        case FillStatus::NONE: return "NONE";
        case FillStatus::PARTIAL: return "PARTIAL";
        case FillStatus::FILLED: return "FILLED";
    }
    return "UNKNOWN";
}

// The engine's result, in the same shape as the model's prediction.
struct ActualOutcome {
    ExpectedOutcome outcome;
    OrderId submissionId = 0;
};

std::string formatOutcome(const ExpectedOutcome& outcome, CommandType type) {
    std::string text;
    if (outcome.success) {
        text = type == CommandType::SUBMIT ? "ACCEPTED" : "CANCELLED";
    } else {
        text = std::string("REJECTED reason=") +
               (outcome.rejection ? rejectionReasonName(*outcome.rejection) : "(none given)");
    }
    if (type == CommandType::SUBMIT) {
        text += " remaining=" + std::to_string(outcome.remaining) + " fill=" + fillStatusName(outcome.fillStatus);
    }
    text += "\n";
    for (const BookEvent& event : outcome.events) {
        text += "    " + formatEvent(event) + "\n";
    }
    return text;
}

std::optional<std::string> compareOutcomes(const ExpectedOutcome& expected, const ActualOutcome& actual,
                                           const Command& command) {
    const ExpectedOutcome& got = actual.outcome;
    const bool submission = command.type == CommandType::SUBMIT;
    const bool same = expected.success == got.success && expected.rejection == got.rejection &&
                      expected.events == got.events &&
                      (!submission || (expected.remaining == got.remaining && expected.fillStatus == got.fillStatus &&
                                       actual.submissionId == command.orderId));
    if (same) {
        return std::nullopt;
    }
    std::string reason = "expected (reference model):\n  " + formatOutcome(expected, command.type) +
                         "actual (order book):\n  " + formatOutcome(got, command.type);
    if (submission && actual.submissionId != command.orderId) {
        reason += "the result's submission ID is " + std::to_string(actual.submissionId) + ", not " +
                  std::to_string(command.orderId) + "\n";
    }
    const auto [expectedEvent, gotEvent] =
        std::mismatch(expected.events.begin(), expected.events.end(), got.events.begin(), got.events.end());
    if (expectedEvent != expected.events.end() || gotEvent != got.events.end()) {
        reason += "the events first differ at position " + std::to_string(expectedEvent - expected.events.begin()) + "\n";
    }
    return reason;
}

template <typename Levels, typename ModelLevels>
std::optional<std::string> compareLevels(const Levels& levels, const ModelLevels& modelLevels,
                                         const ReferenceBook& model, const char* side) {
    auto modelLevel = modelLevels.begin();
    std::size_t position = 0;
    for (const auto& [price, level] : levels) {
        const std::string where = std::string(side) + " level " + std::to_string(position);
        if (modelLevel == modelLevels.end()) {
            return where + ": the book has a level at " + std::to_string(price) + " the model lacks";
        }
        const auto& [modelPrice, modelQueue] = *modelLevel;
        if (price != modelPrice) {
            return where + ": the book's price is " + std::to_string(price) + ", the model's " +
                   std::to_string(modelPrice);
        }
        const order_book::OrderQueue& queue = Access::orders(level);
        if (queue.size() != modelQueue.size()) {
            return where + " at " + std::to_string(price) + ": the book queues " + std::to_string(queue.size()) +
                   " orders, the model " + std::to_string(modelQueue.size());
        }
        std::uint64_t modelTotal = 0;
        std::size_t place = 0;
        auto modelId = modelQueue.begin();
        for (const Order& order : queue) {
            const Quantity modelOpen = model.find(*modelId)->open;
            if (order.getOrderId() != *modelId || order.getRemainingQuantity() != modelOpen) {
                return where + " at " + std::to_string(price) + ", queue position " + std::to_string(place) +
                       ": the book has #" + std::to_string(order.getOrderId()) + " open " +
                       std::to_string(order.getRemainingQuantity()) + ", the model #" + std::to_string(*modelId) +
                       " open " + std::to_string(modelOpen);
            }
            modelTotal += modelOpen;
            ++modelId;
            ++place;
        }
        if (level.getAggregateQuantity() != modelTotal) {
            return where + " at " + std::to_string(price) + ": the book's aggregate is " +
                   std::to_string(level.getAggregateQuantity()) + ", the model's " + std::to_string(modelTotal);
        }
        ++modelLevel;
        ++position;
    }
    if (modelLevel != modelLevels.end()) {
        return std::string(side) + ": the model has a level at " + std::to_string(modelLevel->first) +
               " the book lacks";
    }
    return std::nullopt;
}

// Every level, in order, with the same aggregate and the same FIFO queue of IDs and open
// quantities. Together with checkInvariants, which ties the index to the levels, this pins
// down the whole book.
std::optional<std::string> compareState(const OrderBook& book, const ReferenceBook& model) {
    if (book.activeOrderCount() != model.restingCount()) {
        return "the book rests " + std::to_string(book.activeOrderCount()) + " orders, the model " +
               std::to_string(model.restingCount());
    }
    if (auto difference = compareLevels(Access::bids(book), model.bidQueues(), model, "bids")) {
        return difference;
    }
    return compareLevels(Access::asks(book), model.askQueues(), model, "asks");
}

std::string describeQueue(std::uint64_t total, const std::vector<std::pair<OrderId, Quantity>>& orders,
                          Price price) {
    std::string text = std::to_string(price) + " x" + std::to_string(total) + " [";
    for (std::size_t i = 0; i < orders.size() && i < shownOrders; ++i) {
        text += (i == 0 ? "#" : " #") + std::to_string(orders[i].first) + "/" + std::to_string(orders[i].second);
    }
    if (orders.size() > shownOrders) {
        text += " +" + std::to_string(orders.size() - shownOrders) + " more";
    }
    return text + "]";
}

template <typename Levels>
std::string describeBookSide(const Levels& levels) {
    std::string text;
    std::size_t shown = 0;
    for (const auto& [price, level] : levels) {
        if (shown == shownLevels) {
            text += " | +" + std::to_string(levels.size() - shownLevels) + " more levels";
            break;
        }
        std::vector<std::pair<OrderId, Quantity>> orders;
        for (const Order& order : Access::orders(level)) {
            orders.emplace_back(order.getOrderId(), order.getRemainingQuantity());
        }
        text += (shown == 0 ? "" : " | ") + describeQueue(level.getAggregateQuantity(), orders, price);
        ++shown;
    }
    return text.empty() ? "(empty)" : text;
}

std::string describeModelSide(const std::vector<ModelLevel>& levels) {
    std::string text;
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (i == shownLevels) {
            text += " | +" + std::to_string(levels.size() - shownLevels) + " more levels";
            break;
        }
        text += (i == 0 ? "" : " | ") + describeQueue(levels[i].totalQuantity, levels[i].orders, levels[i].price);
    }
    return text.empty() ? "(empty)" : text;
}

// Finds an order by walking the levels, so a broken index cannot mislead or crash it.
template <typename Levels>
std::optional<std::string> locateInLevels(const Levels& levels, OrderId orderId, Side side) {
    for (const auto& [price, level] : levels) {
        std::size_t place = 0;
        for (const Order& order : Access::orders(level)) {
            if (order.getOrderId() == orderId) {
                return std::string("resting ") + sideName(side) + " at " + std::to_string(price) + ", queue position " +
                       std::to_string(place) + ", open " + std::to_string(order.getRemainingQuantity()) + " of " +
                       std::to_string(order.getInitialQuantity()) + ", sequence " +
                       std::to_string(order.getSequenceNum());
            }
            ++place;
        }
    }
    return std::nullopt;
}

// The behavior properties, judged from the engine's result and book alone.
std::optional<std::string> checkBehavior(const OrderBook& book, const Command& command, const ActualOutcome& actual) {
    if (command.type != CommandType::SUBMIT || command.overload != SubmitOverload::REQUEST || !actual.outcome.success) {
        return std::nullopt;
    }
    if (command.postOnly) {
        for (const BookEvent& event : actual.outcome.events) {
            if (std::holds_alternative<TradeExecuted>(event)) {
                return "a post-only order took liquidity: " + formatEvent(event);
            }
        }
    }
    if (command.orderType == OrderType::MARKET) {
        const std::vector<BookEvent>& events = actual.outcome.events;
        const auto* acceptance = events.empty() ? nullptr : std::get_if<OrderAccepted>(&events.front());
        if (acceptance == nullptr || acceptance->price != 0) {
            return "a market order's acceptance should report price 0";
        }
    }
    if (command.timeInForce == TimeInForce::IOC || command.timeInForce == TimeInForce::FOK) {
        const char* kind = command.timeInForce == TimeInForce::IOC ? "an IOC" : "a FOK";
        std::optional<std::string> resting = locateInLevels(Access::bids(book), command.orderId, Side::BUY);
        if (!resting) {
            resting = locateInLevels(Access::asks(book), command.orderId, Side::SELL);
        }
        if (resting) {
            return std::string(kind) + " order rests: " + *resting;
        }
        const std::vector<BookEvent>& events = actual.outcome.events;
        const Quantity remaining = actual.outcome.remaining;
        const bool expiredLast = !events.empty() && events.back() == BookEvent{OrderExpired{command.orderId, remaining}};
        if (remaining > 0 && !expiredLast) {
            return std::string(kind) + " order's unfilled " + std::to_string(remaining) +
                   " should end its events as an OrderExpired for that quantity";
        }
        if (command.timeInForce == TimeInForce::FOK && remaining > 0 && remaining != command.quantity) {
            return "a FOK order traded part of its quantity: " + std::to_string(command.quantity - remaining) + " of " +
                   std::to_string(command.quantity);
        }
    }
    return std::nullopt;
}

std::size_t countLevels(const OrderBook& book) {
    return Access::bids(book).size() + Access::asks(book).size();
}

}  // namespace

const char* failureTypeName(FailureType type) {
    switch (type) {
        case FailureType::UNEXPECTED_EXCEPTION: return "UNEXPECTED_EXCEPTION";
        case FailureType::INVARIANT_VIOLATION: return "INVARIANT_VIOLATION";
        case FailureType::BEHAVIOR_VIOLATION: return "BEHAVIOR_VIOLATION";
        case FailureType::OUTCOME_MISMATCH: return "OUTCOME_MISMATCH";
        case FailureType::BOOK_STATE_MISMATCH: return "BOOK_STATE_MISMATCH";
        case FailureType::PROPERTY_VIOLATION: return "PROPERTY_VIOLATION";
    }
    return "UNKNOWN";
}

std::string RunStats::summary() const {
    return "commands=" + std::to_string(commands) + " submissions=" + std::to_string(submissions) + " (accepted " +
           std::to_string(accepted) + ", rejected " + std::to_string(rejected) + ") cancellations=" +
           std::to_string(cancellations) + " (cancelled " + std::to_string(cancelled) + ", rejected " +
           std::to_string(cancelsRejected) + ") intentionally-invalid=" + std::to_string(intentionallyInvalid) +
           " trades=" + std::to_string(trades) + " (at most " + std::to_string(mostTradesInOneSubmission) +
           " per submission) expired=" + std::to_string(expired) + " peak-resting=" + std::to_string(peakResting) +
           " peak-levels=" + std::to_string(peakLevels);
}

std::string describeBookState(const OrderBook& book, const ReferenceBook& model, const Command& command) {
    std::ostringstream out;
    out << "  book:  " << book.activeOrderCount() << " indexed orders, " << countLevels(book) << " levels\n";
    out << "    bids " << describeBookSide(Access::bids(book)) << "\n";
    out << "    asks " << describeBookSide(Access::asks(book)) << "\n";
    out << "  model: " << model.restingCount() << " resting orders, " << model.levelCount() << " levels\n";
    out << "    bids " << describeModelSide(model.levels(Side::BUY)) << "\n";
    out << "    asks " << describeModelSide(model.levels(Side::SELL)) << "\n";
    if (command.orderId != 0) {
        const OrderId orderId = command.orderId;
        std::optional<std::string> inBook = locateInLevels(Access::bids(book), orderId, Side::BUY);
        if (!inBook) {
            inBook = locateInLevels(Access::asks(book), orderId, Side::SELL);
        }
        std::string inModel = "not resting";
        if (const ModelOrder* order = model.find(orderId)) {
            inModel = std::string("resting ") + sideName(order->side) + " at " + std::to_string(order->price) +
                      ", open " + std::to_string(order->open);
        } else if (model.removedIds().contains(orderId)) {
            inModel = "not resting (filled or cancelled earlier)";
        }
        out << "  order #" << orderId << ": book " << inBook.value_or("not resting") << "; model " << inModel << "\n";
    }
    return out.str();
}

RunResult CommandReplayer::replay(OrderBook& book, const std::vector<Command>& commands) const {
    if (book.activeOrderCount() != 0) {
        throw std::invalid_argument("CommandReplayer::replay needs an empty book: the reference model starts empty");
    }
    ReferenceBook model;
    RunResult result;
    for (std::size_t i = 0; i < commands.size(); ++i) {
        if (std::optional<Failure> failure = execute(book, model, commands[i], i, result.stats)) {
            result.failure = std::move(failure);
            break;
        }
    }
    return result;
}

RunResult CommandReplayer::run(OrderBook& book, RandomCommandGenerator& generator, std::size_t count,
                               CommandRecorder& recorder) const {
    if (book.activeOrderCount() != 0) {
        throw std::invalid_argument("CommandReplayer::run needs an empty book: the reference model starts empty");
    }
    ReferenceBook model;
    RunResult result;
    for (std::size_t i = 0; i < count; ++i) {
        const Command command = generator.next(model);
        recorder.record(command);
        if (std::optional<Failure> failure = execute(book, model, command, i, result.stats)) {
            result.failure = std::move(failure);
            break;
        }
    }
    return result;
}

std::optional<Failure> CommandReplayer::execute(OrderBook& book, ReferenceBook& model, const Command& command,
                                                std::size_t index, RunStats& stats) const {
    noteCommand(index, command);
    ++stats.commands;
    if (isIntentionallyInvalid(command.intent)) {
        ++stats.intentionallyInvalid;
    }
    const bool submission = command.type == CommandType::SUBMIT;
    ++(submission ? stats.submissions : stats.cancellations);

    const ExpectedOutcome expected = applyCommand(model, command);

    auto fail = [&](FailureType type, std::string reason, std::optional<InvariantViolation> violation = {}) {
        return Failure{type, index, command, violation, std::move(reason), describeBookState(book, model, command)};
    };

    ActualOutcome actual;
    const char* operation = submission ? "submitOrder" : "cancelOrder";
    try {
        if (submission) {
            const SubmissionResult result = command.overload == SubmitOverload::ORDER
                                                ? book.submitOrder(Access::build(command))
                                                : book.submitOrder(requestOf(command));
            actual.submissionId = result.getSubmissionId();
            actual.outcome.success = result.getStatus() == SubmissionStatus::ACCEPTED;
            if (!actual.outcome.success) {
                actual.outcome.rejection = result.getRejectionReason();
            }
            actual.outcome.events = result.getEvents().toVector();
            actual.outcome.remaining = result.getRemainingQuantity();
            actual.outcome.fillStatus = result.getFillStatus();
        } else {
            const CancellationResult result = book.cancelOrder(command.orderId);
            actual.outcome.success = result.isSuccess();
            actual.outcome.rejection = result.getRejectionReason();
            actual.outcome.events = result.getEvents().toVector();
        }
    } catch (const std::exception& error) {
        return fail(FailureType::UNEXPECTED_EXCEPTION, std::string(operation) + " threw: " + error.what());
    } catch (...) {
        return fail(FailureType::UNEXPECTED_EXCEPTION, std::string(operation) + " threw a non-standard exception");
    }

    // With automatic checks on, the command's own boundary has just run this same check on this
    // same state, aborting on a violation (the abort reporter then names the command), so only
    // run it where the engine does not.
    const InvariantResult invariants = invariantChecksAfterCommands ? InvariantResult{} : book.checkInvariants();
    if (!invariants.isValid()) {
        return fail(FailureType::INVARIANT_VIOLATION, formatInvariantResult(invariants), invariants.violation);
    }
    if (std::optional<std::string> broken = checkBehavior(book, command, actual)) {
        return fail(FailureType::BEHAVIOR_VIOLATION, *broken);
    }
    if (std::optional<std::string> difference = compareOutcomes(expected, actual, command)) {
        return fail(FailureType::OUTCOME_MISMATCH, *difference);
    }
    if (std::optional<std::string> difference = compareState(book, model)) {
        return fail(FailureType::BOOK_STATE_MISMATCH, *difference);
    }
    if (property) {
        if (std::optional<std::string> reason = property(book, command)) {
            return fail(FailureType::PROPERTY_VIOLATION, *reason);
        }
    }

    if (submission) {
        ++(expected.success ? stats.accepted : stats.rejected);
        const auto count = [&](auto isKind) {
            return static_cast<std::size_t>(std::count_if(expected.events.begin(), expected.events.end(), isKind));
        };
        const std::size_t trades = count([](const BookEvent& event) { return std::holds_alternative<TradeExecuted>(event); });
        stats.expired += count([](const BookEvent& event) { return std::holds_alternative<OrderExpired>(event); });
        stats.trades += trades;
        stats.mostTradesInOneSubmission = std::max(stats.mostTradesInOneSubmission, trades);
    } else {
        ++(expected.success ? stats.cancelled : stats.cancelsRejected);
    }
    stats.peakResting = std::max(stats.peakResting, model.restingCount());
    stats.peakLevels = std::max(stats.peakLevels, model.levelCount());
    return std::nullopt;
}

}  // namespace order_book::randomized
