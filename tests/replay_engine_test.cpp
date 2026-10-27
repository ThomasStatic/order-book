#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <utility>
#include <stdexcept>
#include <streambuf>
#include <istream>
#include <string>
#include <variant>
#include <vector>

#include "order_book/book_event.hpp"
#include "order_book/event_format.hpp"
#include "order_book/invariants.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#include "order_book/replay_engine.hpp"
#include "order_book/replay_parser.hpp"
#include "order_book/replay_summary.hpp"

namespace order_book {

// Befriended by OrderBook and PriceLevel so these tests can break the book on purpose.
struct OrderBookTestAccess {
    static std::uint64_t& aggregate(OrderBook& book, Price askPrice) { return book.asks.at(askPrice).quantity; }
    static Price& indexedPrice(OrderBook& book, OrderId orderId) { return book.orderIndex.at(orderId).tickPrice; }
};

}  // namespace order_book

namespace {

using order_book::CancelRejected;
using order_book::Order;
using order_book::OrderAccepted;
using order_book::OrderBook;
using order_book::OrderCancelled;
using order_book::OrderRejected;
using order_book::PriceLevelSummary;
using order_book::RejectionReason;
using order_book::Side;
using order_book::TradeExecuted;
using order_book::replay::AddCommand;
using order_book::replay::CancelCommand;
using order_book::replay::InvariantValidation;
using order_book::replay::ReplayOptions;
using order_book::replay::readReplayFile;
using order_book::replay::readReplayInput;
using order_book::replay::ReplayErrorCode;
using order_book::replay::ReplayInput;
using order_book::replay::ReplayEngine;
using order_book::replay::ReplayError;
using order_book::replay::ReplayEvent;
using order_book::replay::ReplayOutcome;
using order_book::replay::ReplayRecord;
using order_book::replay::ReplayResult;
using order_book::replay::ReplayStatistics;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

std::string describe(const ReplayEvent& event) {
    return "line " + std::to_string(event.lineNumber) + " " + order_book::formatEvent(event.event);
}

std::string describe(const ReplayStatistics& s) {
    return "commands=" + std::to_string(s.commandsProcessed) + " adds=" + std::to_string(s.addCommands)
         + " cancels=" + std::to_string(s.cancelCommands) + " addsAccepted=" + std::to_string(s.addsAccepted)
         + " addsRejected=" + std::to_string(s.addsRejected) + " cancelsAccepted=" + std::to_string(s.cancelsAccepted)
         + " cancelsRejected=" + std::to_string(s.cancelsRejected) + " trades=" + std::to_string(s.tradesExecuted)
         + " volume=" + std::to_string(s.volumeTraded);
}

std::string describe(const std::vector<PriceLevelSummary>& levels) {
    if (levels.empty()) {
        return "none";
    }
    std::string text;
    for (const PriceLevelSummary& level : levels) {
        text += (text.empty() ? "" : ", ") + std::to_string(level.price) + " x " + std::to_string(level.totalQuantity)
              + " (" + std::to_string(level.orderCount) + " orders)";
    }
    return text;
}

ReplayResult expectResult(const ReplayOutcome& outcome, const std::string& context) {
    if (const ReplayError* error = std::get_if<ReplayError>(&outcome)) {
        expect(false, context + ": unexpected replay error: " + error->message);
    }
    return std::get<ReplayResult>(outcome);
}

void expectEvents(const ReplayResult& result, const std::vector<ReplayEvent>& expected, const std::string& context) {
    for (std::size_t i = 0; i < result.events.size() && i < expected.size(); ++i) {
        expect(result.events[i] == expected[i], context + ": event " + std::to_string(i) + " is '"
               + describe(result.events[i]) + "', expected '" + describe(expected[i]) + "'");
    }
    expect(result.events.size() == expected.size(), context + ": " + std::to_string(result.events.size())
           + " events, expected " + std::to_string(expected.size()));
}

void expectStatistics(const ReplayResult& result, const ReplayStatistics& expected, const std::string& context) {
    expect(result.statistics == expected, context + ": statistics are " + describe(result.statistics)
           + ", expected " + describe(expected));
}

void expectLevels(const std::vector<PriceLevelSummary>& actual, const std::vector<PriceLevelSummary>& expected,
                  const std::string& context) {
    bool same = actual.size() == expected.size();
    for (std::size_t i = 0; same && i < actual.size(); ++i) {
        same = actual[i].price == expected[i].price && actual[i].totalQuantity == expected[i].totalQuantity
            && actual[i].orderCount == expected[i].orderCount;
    }
    expect(same, context + ": levels are " + describe(actual) + ", expected " + describe(expected));
}

void expectFinalBook(const ReplayResult& result, const std::vector<PriceLevelSummary>& bids,
                     const std::vector<PriceLevelSummary>& asks, const std::string& context) {
    expectLevels(result.finalBook.bids, bids, context + ": final bids");
    expectLevels(result.finalBook.asks, asks, context + ": final asks");
    unsigned int orders = 0;
    for (const PriceLevelSummary& level : bids) {
        orders += level.orderCount;
    }
    for (const PriceLevelSummary& level : asks) {
        orders += level.orderCount;
    }
    expect(result.finalBook.totalActiveVisibleOrders == orders, context + ": wrong resting order count");
}

constexpr unsigned int maxField = std::numeric_limits<unsigned int>::max();

// The example from docs/replay-format.md: rest, rest, cross, cancel.
const std::vector<ReplayRecord> basicRecords = {
    ReplayRecord{2, 1, AddCommand{1001, Side::BUY, 100, 50}},
    ReplayRecord{3, 2, AddCommand{1002, Side::SELL, 105, 30}},
    ReplayRecord{4, 3, AddCommand{1003, Side::BUY, 105, 20}},
    ReplayRecord{5, 4, CancelCommand{1001}}};

void testBasicReplay() {
    OrderBook book;
    const ReplayResult result = expectResult(ReplayEngine::run(basicRecords, book), "basic");

    expectEvents(result, {
        {2, OrderAccepted{1001, Side::BUY, 100, 50}},
        {3, OrderAccepted{1002, Side::SELL, 105, 30}},
        {4, OrderAccepted{1003, Side::BUY, 105, 20}},
        {4, TradeExecuted{1003, 1002, 105, 20}},
        {5, OrderCancelled{1001, 50}}}, "basic");
    expectStatistics(result, ReplayStatistics{4, 3, 1, 3, 0, 1, 0, 1, 20}, "basic");
    expectFinalBook(result, {}, {{105, 10, 1}}, "basic");
    expect(book.activeOrderCount() == 1 && book.findActiveOrder(1002).getRemainingQuantity() == 10,
           "basic: 1002 should rest alone with 10 left");
}

// The same records on two fresh books give the same events, statistics, and final book.
void testReplayIsRepeatable() {
    OrderBook first;
    OrderBook second;
    const ReplayResult a = expectResult(ReplayEngine::run(basicRecords, first), "first run");
    const ReplayResult b = expectResult(ReplayEngine::run(basicRecords, second), "second run");
    expect(a.events == b.events, "repeat: events differ");
    expect(a.statistics == b.statistics, "repeat: statistics differ");
    expectFinalBook(b, a.finalBook.bids, a.finalBook.asks, "repeat");
}

ReplayError expectError(const ReplayOutcome& outcome, ReplayErrorCode code, std::optional<std::size_t> line,
                        const std::string& context) {
    const ReplayError* error = std::get_if<ReplayError>(&outcome);
    expect(error != nullptr, context + ": expected a replay error");
    expect(error->code == code, context + ": wrong error code " + order_book::replay::replayErrorCodeName(error->code));
    expect(error->lineNumber == line, context + ": wrong error line");
    expect(!error->parseError.has_value(), context + ": not a parse error");
    return *error;
}

ReplayOptions validation(InvariantValidation mode) {
    ReplayOptions options;
    options.invariantValidation = mode;
    return options;
}

// An ask of 5 at 200 (order 1) whose level caches 6, which the basic records leave alone.
OrderBook bookWithBrokenAggregate() {
    OrderBook book;
    book.addOrder(Order(1, 200, 5, 1, Side::SELL));
    order_book::OrderBookTestAccess::aggregate(book, 200) = 6;
    return book;
}

void testInvariantValidation() {
    expect(ReplayOptions{}.invariantValidation == InvariantValidation::FINAL, "FINAL should be the default");

    // No command runs, so the book's own boundary never sees the damage: only run can.
    {
        OrderBook book = bookWithBrokenAggregate();
        expectResult(ReplayEngine::run({}, book, validation(InvariantValidation::NONE)), "NONE, no commands");
    }
    for (InvariantValidation mode : {InvariantValidation::FINAL, InvariantValidation::PER_COMMAND}) {
        OrderBook book = bookWithBrokenAggregate();
        const ReplayError error = expectError(ReplayEngine::run({}, book, validation(mode)),
                                              ReplayErrorCode::INVARIANT_VIOLATION, std::nullopt, "no commands");
        expect(error.message.find("AGGREGATE_QUANTITY_MISMATCH") != std::string::npos,
               "the message should name the violation: " + error.message);
    }

    // A sound book passes every mode.
    for (InvariantValidation mode : {InvariantValidation::NONE, InvariantValidation::FINAL, InvariantValidation::PER_COMMAND}) {
        OrderBook book;
        expectResult(ReplayEngine::run(basicRecords, book, validation(mode)), "sound book");
    }

    // With automatic checks on, a command on a broken book aborts at its own boundary.
    if constexpr (!order_book::invariantChecksAfterCommands) {
        {
            OrderBook book = bookWithBrokenAggregate();
            const ReplayResult result = expectResult(
                ReplayEngine::run(basicRecords, book, validation(InvariantValidation::NONE)), "NONE");
            expectFinalBook(result, {}, {{105, 10, 1}, {200, 6, 1}}, "NONE reports the damaged level as it is");
        }
        {
            OrderBook book = bookWithBrokenAggregate();
            const ReplayError error = expectError(
                ReplayEngine::run(basicRecords, book, validation(InvariantValidation::FINAL)),
                ReplayErrorCode::INVARIANT_VIOLATION, std::nullopt, "FINAL");
            expect(error.message.find("AGGREGATE_QUANTITY_MISMATCH") != std::string::npos, "FINAL: " + error.message);
            // Every command ran before the final check.
            expect(book.activeOrderCount() == 2, "FINAL: every command should have run");
        }
        {
            OrderBook book = bookWithBrokenAggregate();
            const ReplayError error = expectError(
                ReplayEngine::run(basicRecords, book, validation(InvariantValidation::PER_COMMAND)),
                ReplayErrorCode::INVARIANT_VIOLATION, 2, "PER_COMMAND");
            expect(error.message.find("AGGREGATE_QUANTITY_MISMATCH") != std::string::npos,
                   "PER_COMMAND: " + error.message);
            // It stopped after the first command: only 1001 joined order 1.
            expect(book.activeOrderCount() == 2 && book.findActiveOrder(1001).getRemainingQuantity() == 50,
                   "PER_COMMAND: nothing after line 2 should have run");
        }
    }
}

// A book that throws stops the replay at that command's line.
void testExecutionFailure() {
    if constexpr (!order_book::invariantChecksAfterCommands) {
        OrderBook book;
        book.addOrder(Order(1, 90, 4, 1, Side::BUY));
        // Order 1's entry names a price with no level, so cancelOrder throws.
        order_book::OrderBookTestAccess::indexedPrice(book, 1) = 91;
        const std::vector<ReplayRecord> records = {
            ReplayRecord{2, 1, AddCommand{2, Side::SELL, 100, 3}},
            ReplayRecord{3, 2, CancelCommand{1}},
            ReplayRecord{4, 3, CancelCommand{2}}};
        const ReplayError error = expectError(
            ReplayEngine::run(records, book, validation(InvariantValidation::NONE)),
            ReplayErrorCode::INTERNAL_EXECUTION_FAILURE, 3, "throwing cancel");
        expect(!error.message.empty(), "the failure should say what the book threw");
        // Line 4 never ran: order 2 still rests.
        expect(book.activeOrderCount() == 2, "nothing after the failing command should run");
    }
}

void expectText(const std::string& actual, const std::string& expected, const std::string& context) {
    expect(actual == expected, context + ": got\n" + actual + "\nexpected\n" + expected);
}

void testEventFormatting() {
    using order_book::replay::formatReplayEvent;
    expectText(formatReplayEvent({2, OrderAccepted{1001, Side::BUY, 100, 50}}),
               "line 2: OrderAccepted id=1001 side=BUY price=100 qty=50", "accepted");
    expectText(formatReplayEvent({4, TradeExecuted{1003, 1002, 105, 20}}),
               "line 4: TradeExecuted incoming=1003 resting=1002 price=105 qty=20", "trade");
    expectText(formatReplayEvent({123456789, CancelRejected{9999, RejectionReason::ORDER_NOT_FOUND}}),
               "line 123456789: CancelRejected id=9999 reason=ORDER_NOT_FOUND", "large line number, no grouping");
}

void testSummaryFormatting() {
    using order_book::replay::formatReplaySummary;
    {
        OrderBook book;
        expectText(formatReplaySummary(expectResult(ReplayEngine::run(basicRecords, book), "basic")),
                   "commands processed: 4\n"
                   "add commands: 3\n"
                   "adds accepted: 3\n"
                   "adds rejected: 0\n"
                   "cancel commands: 1\n"
                   "cancels accepted: 1\n"
                   "cancels rejected: 0\n"
                   "trades executed: 1\n"
                   "volume traded: 20\n"
                   "resting orders: 1\n"
                   "bids:\n"
                   "  none\n"
                   "asks:\n"
                   "  price=105 qty=10 orders=1\n",
                   "basic summary");
    }
    {
        // The summary is the statistics, then the final book, which the CLI prints separately.
        OrderBook book;
        const ReplayResult result = expectResult(ReplayEngine::run(basicRecords, book), "basic parts");
        const std::string statistics = order_book::replay::formatReplayStatistics(result);
        const std::string finalBook = order_book::replay::formatFinalBook(result.finalBook);
        expect(statistics.ends_with("resting orders: 1\n"), "statistics end with resting orders: " + statistics);
        expectText(finalBook, "bids:\n  none\nasks:\n  price=105 qty=10 orders=1\n", "basic final book");
        expectText(formatReplaySummary(result), statistics + finalBook, "summary is statistics then book");
    }
    {
        OrderBook book;
        expectText(formatReplaySummary(expectResult(ReplayEngine::run({}, book), "empty")),
                   "commands processed: 0\n"
                   "add commands: 0\n"
                   "adds accepted: 0\n"
                   "adds rejected: 0\n"
                   "cancel commands: 0\n"
                   "cancels accepted: 0\n"
                   "cancels rejected: 0\n"
                   "trades executed: 0\n"
                   "volume traded: 0\n"
                   "resting orders: 0\n"
                   "bids:\n"
                   "  none\n"
                   "asks:\n"
                   "  none\n",
                   "empty summary");
    }
    {
        // Levels entered out of price order print best first: bids descending, asks ascending.
        const std::vector<ReplayRecord> records = {
            ReplayRecord{2, 1, AddCommand{1, Side::BUY, 99, 5}},
            ReplayRecord{3, 2, AddCommand{2, Side::BUY, 100, 7}},
            ReplayRecord{4, 3, AddCommand{3, Side::BUY, 98, 1}},
            ReplayRecord{5, 4, AddCommand{4, Side::BUY, 100, 3}},
            ReplayRecord{6, 5, AddCommand{5, Side::SELL, 103, 2}},
            ReplayRecord{7, 6, AddCommand{6, Side::SELL, 101, 4}},
            ReplayRecord{8, 7, AddCommand{7, Side::SELL, 102, 6}}};
        OrderBook book;
        const std::string summary = formatReplaySummary(expectResult(ReplayEngine::run(records, book), "levels"));
        expectText(summary.substr(summary.find("resting orders")),
                   "resting orders: 7\n"
                   "bids:\n"
                   "  price=100 qty=10 orders=2\n"
                   "  price=99 qty=5 orders=1\n"
                   "  price=98 qty=1 orders=1\n"
                   "asks:\n"
                   "  price=101 qty=4 orders=1\n"
                   "  price=102 qty=6 orders=1\n"
                   "  price=103 qty=2 orders=1\n",
                   "level order");
    }
    {
        // Totals beyond one Quantity print in full.
        ReplayResult result;
        result.statistics.volumeTraded = 8589934590;  // 2 * 4294967295
        result.finalBook.asks.push_back(PriceLevelSummary{maxField, 8589934590, 2});
        result.finalBook.totalActiveVisibleOrders = 2;
        const std::string summary = formatReplaySummary(result);
        expect(summary.find("volume traded: 8589934590\n") != std::string::npos, "large volume: " + summary);
        expect(summary.find("  price=4294967295 qty=8589934590 orders=2\n") != std::string::npos,
               "large level: " + summary);
    }
}

void testErrorFormatting() {
    using order_book::replay::formatParseError;
    using order_book::replay::formatReplayError;
    using order_book::replay::ParseError;

    const ParseError parseError{4, order_book::replay::ParseErrorCode::INVALID_VALUE, "price", "0",
                                "price must be positive, not '0'"};
    expectText(formatReplayError(ReplayError{ReplayErrorCode::INPUT_READ_FAILURE, 4, parseError.message, parseError},
                                 "orders.csv"),
               formatParseError(parseError, "orders.csv"), "parse error prints as the parser's diagnostic");
    expectText(formatReplayError(ReplayError{ReplayErrorCode::INPUT_READ_FAILURE, std::nullopt,
                                             "cannot open replay file", std::nullopt}, "missing.csv"),
               "missing.csv: cannot open replay file", "unopenable file");
    expectText(formatReplayError(ReplayError{ReplayErrorCode::INTERNAL_EXECUTION_FAILURE, 3,
                                             "the book threw: boom", std::nullopt}, "orders.csv"),
               "orders.csv:3: engine failure: the book threw: boom", "exception");
    expectText(formatReplayError(ReplayError{ReplayErrorCode::INVARIANT_VIOLATION, std::nullopt,
                                             "invariant violated after the last command: X", std::nullopt}, "orders.csv"),
               "orders.csv: engine failure: invariant violated after the last command: X", "final check");
}

struct ExpectedReplay {
    std::vector<ReplayEvent> events;
    ReplayStatistics statistics;
    std::vector<PriceLevelSummary> bids;
    std::vector<PriceLevelSummary> asks;
};

// Each valid sample's events and final book as tests/replay/README.md lists them.
void testValidSampleFiles(const std::filesystem::path& root) {
    const std::map<std::string, ExpectedReplay> valid = {
        {"basic.csv", {{{2, OrderAccepted{1001, Side::BUY, 100, 50}},
                        {3, OrderAccepted{1002, Side::SELL, 105, 30}},
                        {4, OrderAccepted{1003, Side::BUY, 105, 20}},
                        {4, TradeExecuted{1003, 1002, 105, 20}},
                        {5, OrderCancelled{1001, 50}}},
                       {4, 3, 1, 3, 0, 1, 0, 1, 20}, {}, {{105, 10, 1}}}},
        {"price_time_priority.csv", {{{2, OrderAccepted{2001, Side::SELL, 101, 10}},
                                      {3, OrderAccepted{2002, Side::SELL, 101, 15}},
                                      {4, OrderAccepted{2003, Side::SELL, 102, 20}},
                                      {5, OrderAccepted{2004, Side::BUY, 99, 5}},
                                      {6, OrderAccepted{2005, Side::BUY, 102, 50}},
                                      {6, TradeExecuted{2005, 2001, 101, 10}},
                                      {6, TradeExecuted{2005, 2002, 101, 15}},
                                      {6, TradeExecuted{2005, 2003, 102, 20}},
                                      {7, OrderAccepted{2006, Side::SELL, 99, 8}},
                                      {7, TradeExecuted{2006, 2005, 102, 5}},
                                      {7, TradeExecuted{2006, 2004, 99, 3}}},
                                     {6, 6, 0, 6, 0, 0, 0, 5, 53}, {{99, 2, 1}}, {}}},
        {"sweep_and_rest.csv", {{{2, OrderAccepted{8001, Side::SELL, 10125, 50}},
                                 {3, OrderAccepted{8002, Side::SELL, 10125, 30}},
                                 {4, OrderAccepted{8003, Side::SELL, 10126, 100}},
                                 {5, OrderAccepted{8004, Side::SELL, 10128, 200}},
                                 {6, OrderAccepted{8005, Side::BUY, 10120, 100}},
                                 {7, OrderAccepted{8006, Side::BUY, 10127, 200}},
                                 {7, TradeExecuted{8006, 8001, 10125, 50}},
                                 {7, TradeExecuted{8006, 8002, 10125, 30}},
                                 {7, TradeExecuted{8006, 8003, 10126, 100}}},
                                {6, 6, 0, 6, 0, 0, 0, 3, 180}, {{10127, 20, 1}, {10120, 100, 1}}, {{10128, 200, 1}}}},
        {"book_rejections.csv", {{{2, OrderAccepted{3001, Side::BUY, 100, 10}},
                                  {3, OrderRejected{3001, RejectionReason::DUPLICATE_ORDER_ID}},
                                  {4, CancelRejected{9999, RejectionReason::ORDER_NOT_FOUND}},
                                  {5, OrderCancelled{3001, 10}},
                                  {6, CancelRejected{3001, RejectionReason::ORDER_NOT_FOUND}},
                                  {7, OrderAccepted{3001, Side::SELL, 110, 5}}},
                                 {6, 3, 3, 2, 1, 1, 2, 0, 0}, {}, {{110, 5, 1}}}},
        {"sequence_gaps.csv", {{{2, OrderAccepted{4001, Side::SELL, 200, 5}},
                                {3, OrderAccepted{4002, Side::SELL, 200, 5}},
                                {4, OrderCancelled{4001, 5}},
                                {5, OrderAccepted{4003, Side::BUY, 200, 5}},
                                {5, TradeExecuted{4003, 4002, 200, 5}}},
                               {4, 3, 1, 3, 0, 1, 0, 1, 5}, {}, {}}},
        {"limits.csv", {{{2, OrderAccepted{maxField, Side::SELL, maxField, maxField}},
                         {3, OrderAccepted{1, Side::BUY, 1, 1}},
                         {4, OrderCancelled{maxField, maxField}}},
                        {3, 2, 1, 2, 0, 1, 0, 0, 0}, {{1, 1, 1}}, {}}},
        {"leading_zeros.csv", {{{2, OrderAccepted{7001, Side::BUY, 100, 10}},
                                {3, OrderAccepted{7002, Side::SELL, 100, 4}},
                                {3, TradeExecuted{7002, 7001, 100, 4}},
                                {4, OrderCancelled{7001, 6}}},
                               {3, 2, 1, 2, 0, 1, 0, 1, 4}, {}, {}}},
        {"header_only.csv", {{}, {}, {}, {}}},
        {"crlf_line_endings.csv", {{{2, OrderAccepted{5001, Side::BUY, 100, 10}},
                                    {3, OrderAccepted{5002, Side::SELL, 100, 4}},
                                    {3, TradeExecuted{5002, 5001, 100, 4}}},
                                   {2, 2, 0, 2, 0, 0, 0, 1, 4}, {{100, 6, 1}}, {}}},
        {"no_final_newline.csv", {{{2, OrderAccepted{6001, Side::SELL, 300, 7}},
                                   {3, OrderCancelled{6001, 7}}},
                                  {2, 1, 1, 1, 0, 1, 0, 0, 0}, {}, {}}},
    };

    std::size_t validSeen = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "valid")) {
        const std::string name = entry.path().filename().string();
        const auto found = valid.find(name);
        expect(found != valid.end(), "valid/" + name + " has no expected replay here");
        const ExpectedReplay& expected = found->second;
        const std::string context = "valid/" + name;

        const ReplayInput input = readReplayFile(entry.path());
        expect(!input.error.has_value(), context + ": unexpected input error: "
               + (input.error ? input.error->message : std::string()));

        OrderBook book;
        const ReplayResult result = expectResult(ReplayEngine::run(input.records, book), context);
        expectEvents(result, expected.events, context);
        expectStatistics(result, expected.statistics, context);
        expectFinalBook(result, expected.bids, expected.asks, context);
        ++validSeen;
    }
    expect(validSeen == valid.size(), "a listed valid sample is missing from " + (root / "valid").string());
}

// Every malformed sample stops at its one error. The records before it are what a replay
// runs, and their effect on the book stands.
void testMalformedSampleFiles(const std::filesystem::path& root) {
    struct StopPoint {
        std::size_t line;
        std::size_t dispatched;
    };
    // Most files share one layout: two valid ADDs, then the bad record on line 4.
    const StopPoint usual{4, 2};
    const std::map<std::string, StopPoint> unusual = {
        {"empty_file.csv", {1, 0}}, {"missing_header.csv", {1, 0}},
        {"header_wrong_order.csv", {1, 0}}, {"header_missing_column.csv", {1, 0}},
        {"header_duplicate_column.csv", {1, 0}}, {"header_extra_column.csv", {1, 0}},
        {"header_wrong_case.csv", {1, 0}}, {"header_with_bom.csv", {1, 0}},
        {"trailing_blank_line.csv", {5, 3}}, {"sequence_zero.csv", {2, 0}}};

    std::size_t unusualSeen = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "malformed")) {
        const std::string name = entry.path().filename().string();
        const std::string context = "malformed/" + name;
        const auto found = unusual.find(name);
        const StopPoint stop = found == unusual.end() ? usual : found->second;
        if (found != unusual.end()) {
            ++unusualSeen;
        }

        const ReplayInput input = readReplayFile(entry.path());
        expect(input.error.has_value(), context + ": expected an input error");
        const ReplayError& error = *input.error;
        expect(error.code == ReplayErrorCode::INPUT_READ_FAILURE, context + ": wrong error code");
        expect(error.lineNumber == stop.line, context + ": wrong error line");
        expect(error.parseError.has_value() && error.parseError->lineNumber == stop.line,
               context + ": should carry the parser's diagnostic");
        expect(error.message == error.parseError->message, context + ": message should be the parser's");
        expect(input.records.size() == stop.dispatched, context + ": wrong number of records before the error");

        OrderBook book;
        const ReplayResult result = expectResult(ReplayEngine::run(input.records, book), context);
        expect(result.statistics.commandsProcessed == stop.dispatched, context + ": wrong number dispatched");
        if (stop.dispatched == 0) {
            expectFinalBook(result, {}, {}, context);
        } else if (stop.dispatched == 2) {
            expectFinalBook(result, {{100, 50, 1}}, {{105, 30, 1}}, context);
        } else {
            // trailing_blank_line.csv's third ADD traded 20 against 1002.
            expectFinalBook(result, {{100, 50, 1}}, {{105, 10, 1}}, context);
        }
    }
    expect(unusualSeen == unusual.size(), "a listed malformed sample is missing from " + (root / "malformed").string());
}

// Delivers its text, then fails: the next read throws from the buffer, which the stream
// turns into badbit, as a disk or network error would.
class FailingStreamBuffer : public std::streambuf {
public:
    explicit FailingStreamBuffer(std::string text) : contents(std::move(text)) {
        setg(contents.data(), contents.data(), contents.data() + contents.size());
    }

protected:
    int_type underflow() override {
        throw std::runtime_error("the device failed");
    }

private:
    std::string contents;
};

void testUnreadableInput(const std::filesystem::path& root) {
    const ReplayInput missing = readReplayFile(root / "no_such_file.csv");
    expect(missing.error.has_value(), "a missing file should be an input error");
    expect(missing.error->code == ReplayErrorCode::INPUT_READ_FAILURE, "missing file: wrong code");
    expect(!missing.error->lineNumber.has_value(), "missing file: no line is at fault");
    expect(!missing.error->parseError.has_value(), "missing file: not a parse error");
    expect(missing.records.empty(), "missing file: no records");

    // A stream that is already bad cannot be read.
    std::istringstream broken("sequence,command,order_id,side,price,quantity\n");
    broken.setstate(std::ios::badbit);
    const ReplayInput bad = readReplayInput(broken);
    expect(bad.error.has_value() && bad.error->code == ReplayErrorCode::INPUT_READ_FAILURE
           && !bad.error->lineNumber.has_value() && !bad.error->parseError.has_value(),
           "a failed stream should be an input error with no line");
    expect(bad.records.empty(), "failed stream: no records");

    // A stream that fails partway: the records completed before the failure stand, and the
    // record it cut short is not read.
    FailingStreamBuffer failing("sequence,command,order_id,side,price,quantity\n"
                                "1,ADD,1001,BUY,100,50\n"
                                "2,ADD,1002,SELL,105,30\n"
                                "3,ADD,10");
    std::istream failingInput(&failing);
    const ReplayInput cut = readReplayInput(failingInput);
    expect(cut.error.has_value() && cut.error->code == ReplayErrorCode::INPUT_READ_FAILURE
           && !cut.error->lineNumber.has_value() && !cut.error->parseError.has_value(),
           "a stream failing partway should be an input error with no line");
    expect(cut.records.size() == 2 && cut.records[1].sequence == 2,
           "the records completed before a stream failure should stand");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "FAILED: usage: replay_engine_tests <path to tests/replay>" << std::endl;
        return EXIT_FAILURE;
    }

    const std::filesystem::path root = argv[1];

    testBasicReplay();
    testReplayIsRepeatable();
    testInvariantValidation();
    testExecutionFailure();
    testEventFormatting();
    testSummaryFormatting();
    testErrorFormatting();
    testValidSampleFiles(root);
    testMalformedSampleFiles(root);
    testUnreadableInput(root);

    std::cout << "replay engine tests passed" << std::endl;
    return EXIT_SUCCESS;
}
