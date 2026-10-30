#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "order_book/book_event.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#include "order_book/order_request.hpp"
#include "order_book/submission_result.hpp"

// The order behaviors of spec #40, through the public API: each test checks the exact result
// and event stream a submission returns, and what it did to the whole book.

namespace order_book {

// Befriended by OrderBook and PriceLevel so these tests can capture the whole book.
struct OrderBookTestAccess {
    static const auto& index(const OrderBook& book) { return book.orderIndex; }
    static const auto& bids(const OrderBook& book) { return book.bids; }
    static const auto& asks(const OrderBook& book) { return book.asks; }
    static TradeId nextTradeId(const OrderBook& book) { return book.nextTradeId; }
    static SequenceNum nextSequenceNum(const OrderBook& book) { return book.nextSequenceNum; }
    static const OrderQueue& orders(const PriceLevel& level) { return level.orders; }
};

}  // namespace order_book

namespace {

using Access = order_book::OrderBookTestAccess;
using order_book::BookEvent;
using order_book::FillStatus;
using order_book::Order;
using order_book::OrderAccepted;
using order_book::OrderBook;
using order_book::OrderExpired;
using order_book::OrderId;
using order_book::OrderRequest;
using order_book::OrderType;
using order_book::Price;
using order_book::SequenceNum;
using order_book::Side;
using order_book::SubmissionResult;
using order_book::SubmissionStatus;
using order_book::TimeInForce;
using order_book::TradeExecuted;
using order_book::TradeId;
using Events = std::vector<BookEvent>;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

std::string sideName(Side side) {
    return side == Side::BUY ? "buy" : "sell";
}

Side opposite(Side side) {
    return side == Side::BUY ? Side::SELL : Side::BUY;
}

// Resting liquidity sits at 100 and further from the incoming side, so an incoming order at
// priceAway(side, n) crosses every resting level up to n ticks away, and at -1 crosses none.
Price priceAway(Side incomingSide, int ticksAway) {
    return static_cast<Price>(incomingSide == Side::BUY ? 100 + ticksAway : 100 - ticksAway);
}

// Everything a submission could disturb, including each order's address.
struct OrderState {
    OrderId id;
    SequenceNum sequenceNum;
    unsigned int remainingQuantity;
    const Order* address;

    bool operator==(const OrderState&) const = default;
};

struct LevelState {
    Price price;
    std::uint64_t aggregateQuantity;
    std::vector<OrderState> orders;

    bool operator==(const LevelState&) const = default;
};

struct BookState {
    std::vector<LevelState> bids;
    std::vector<LevelState> asks;
    std::vector<OrderId> index;
    SequenceNum nextSequenceNum;
    TradeId nextTradeId;

    bool operator==(const BookState&) const = default;
};

template <typename Levels>
std::vector<LevelState> captureLevels(const Levels& levels) {
    std::vector<LevelState> captured;
    for (const auto& [price, level] : levels) {
        LevelState levelState{price, level.getAggregateQuantity(), {}};
        for (const Order& order : Access::orders(level)) {
            levelState.orders.push_back({order.getOrderId(), order.getSequenceNum(), order.getRemainingQuantity(), &order});
        }
        captured.push_back(levelState);
    }
    return captured;
}

BookState capture(const OrderBook& book) {
    std::vector<OrderId> index;
    for (const auto& entry : Access::index(book)) {
        index.push_back(entry.first);
    }
    std::sort(index.begin(), index.end());
    return {captureLevels(Access::bids(book)), captureLevels(Access::asks(book)), index, Access::nextSequenceNum(book),
            Access::nextTradeId(book)};
}

void expectSound(const OrderBook& book, const std::string& context) {
    const order_book::InvariantResult result = book.checkInvariants();
    expect(result.isValid(), context + ": the book should pass its invariant check, got " +
                                 order_book::formatInvariantResult(result));
}

void expectAcceptedWith(const SubmissionResult& result, const Events& events, unsigned int remaining,
                        FillStatus fillStatus, std::size_t trades, const std::string& context) {
    expect(result.getStatus() == SubmissionStatus::ACCEPTED, context + ": should be accepted");
    expect(result.getEvents() == events, context + ": unexpected events");
    expect(result.getRemainingQuantity() == remaining, context + ": unexpected remaining quantity");
    expect(result.getFillStatus() == fillStatus, context + ": unexpected fill status");
    expect(result.getGeneratedTrades().size() == trades, context + ": unexpected number of trades");
}

// Orders 1 (3) and 2 (4) at 100, then 3 (5) one tick further away.
OrderBook seededBook(Side incomingSide) {
    const Side restingSide = opposite(incomingSide);
    OrderBook book;
    book.submitOrder(Order(1, priceAway(incomingSide, 0), 3, 1, restingSide));
    book.submitOrder(Order(2, priceAway(incomingSide, 0), 4, 1, restingSide));
    book.submitOrder(Order(3, priceAway(incomingSide, 1), 5, 1, restingSide));
    return book;
}

OrderRequest ioc(OrderId id, Side side, Price price, unsigned int quantity) {
    return OrderRequest{id, side, price, quantity, OrderType::LIMIT, TimeInForce::IOC};
}

// IOC trades while it crosses, at each resting price, and expires whatever is left; it never
// rests, and it consumes a sequence number like any accepted order.
void testImmediateOrCancel(Side side) {
    const std::string context = "IOC " + sideName(side);
    {
        // Crosses both levels and expires the rest.
        OrderBook book = seededBook(side);
        const SequenceNum sequence = Access::nextSequenceNum(book);
        const SubmissionResult result = book.submitOrder(ioc(9, side, priceAway(side, 1), 15));
        expectAcceptedWith(result,
                           Events{OrderAccepted{9, side, priceAway(side, 1), 15},
                                  TradeExecuted{9, 1, priceAway(side, 0), 3}, TradeExecuted{9, 2, priceAway(side, 0), 4},
                                  TradeExecuted{9, 3, priceAway(side, 1), 5}, OrderExpired{9, 3}},
                           3, FillStatus::PARTIAL, 3, context + " sweep");
        expect(!book.hasBids() && !book.hasAsks() && book.activeOrderCount() == 0,
               context + " sweep: the remainder should not rest");
        expect(Access::nextSequenceNum(book) == sequence + 1, context + " sweep: should consume one sequence number");
        expectSound(book, context + " sweep");
    }
    {
        // Trades at the first level only: its limit stops it short of the second.
        OrderBook book = seededBook(side);
        const SubmissionResult result = book.submitOrder(ioc(9, side, priceAway(side, 0), 10));
        expectAcceptedWith(result,
                           Events{OrderAccepted{9, side, priceAway(side, 0), 10},
                                  TradeExecuted{9, 1, priceAway(side, 0), 3}, TradeExecuted{9, 2, priceAway(side, 0), 4},
                                  OrderExpired{9, 3}},
                           3, FillStatus::PARTIAL, 2, context + " limited");
        expect(book.activeOrderCount() == 1 && book.findActiveOrder(3).getRemainingQuantity() == 5,
               context + " limited: only the level beyond its limit should remain");
        expectSound(book, context + " limited");
    }
    {
        // Filled whole: nothing expires.
        OrderBook book = seededBook(side);
        const SubmissionResult result = book.submitOrder(ioc(9, side, priceAway(side, 0), 5));
        expectAcceptedWith(result,
                           Events{OrderAccepted{9, side, priceAway(side, 0), 5},
                                  TradeExecuted{9, 1, priceAway(side, 0), 3}, TradeExecuted{9, 2, priceAway(side, 0), 2}},
                           0, FillStatus::FILLED, 2, context + " filled");
        expectSound(book, context + " filled");
    }
    {
        // Not crossing: accepted and wholly expired, the book otherwise untouched.
        OrderBook book = seededBook(side);
        BookState before = capture(book);
        const SubmissionResult result = book.submitOrder(ioc(9, side, priceAway(side, -1), 6));
        expectAcceptedWith(result, Events{OrderAccepted{9, side, priceAway(side, -1), 6}, OrderExpired{9, 6}}, 6,
                           FillStatus::NONE, 0, context + " not crossing");
        ++before.nextSequenceNum;
        expect(capture(book) == before, context + " not crossing: only the sequence number should change");
        expectSound(book, context + " not crossing");
    }
    {
        // Facing an empty side: accepted and wholly expired, not rejected.
        OrderBook book;
        const SubmissionResult result = book.submitOrder(ioc(9, side, 100, 6));
        expectAcceptedWith(result, Events{OrderAccepted{9, side, 100, 6}, OrderExpired{9, 6}}, 6, FillStatus::NONE, 0,
                           context + " empty side");
        expect(book.activeOrderCount() == 0 && Access::nextSequenceNum(book) == 2 && Access::nextTradeId(book) == 1,
               context + " empty side: should rest nothing, consume a sequence number, and no trade ID");
    }
}

// An expired order's ID is free again, as a filled one's is.
void testExpiredIdIsFree() {
    OrderBook book;
    book.submitOrder(ioc(9, Side::BUY, 100, 6));
    const SubmissionResult again = book.submitOrder(OrderRequest{9, Side::BUY, 100, 6});
    expect(again.getStatus() == SubmissionStatus::ACCEPTED && book.activeOrderCount() == 1,
           "an expired order's ID should be free for reuse");
}

// A GTC request still rests its remainder, exactly as v1.
void testGoodTillCancelledRests() {
    OrderBook book = seededBook(Side::BUY);
    const SubmissionResult result = book.submitOrder(OrderRequest{9, Side::BUY, 100, 10, OrderType::LIMIT, TimeInForce::GTC});
    expectAcceptedWith(result,
                       Events{OrderAccepted{9, Side::BUY, 100, 10}, TradeExecuted{9, 1, 100, 3}, TradeExecuted{9, 2, 100, 4}},
                       3, FillStatus::PARTIAL, 2, "GTC");
    expect(book.findActiveOrder(9).getRemainingQuantity() == 3, "GTC: the remainder should rest");
}

// The same script on two fresh books gives the same outcomes.
void testScriptIsDeterministic() {
    const auto run = [] {
        OrderBook book;
        std::vector<Events> outcomes;
        const OrderRequest script[] = {
            {1, Side::SELL, 101, 5}, {2, Side::SELL, 102, 5}, ioc(3, Side::BUY, 101, 8), {4, Side::BUY, 99, 4},
            ioc(5, Side::SELL, 98, 10), ioc(6, Side::BUY, 110, 2), ioc(3, Side::SELL, 120, 1), {7, Side::BUY, 102, 9},
        };
        for (const OrderRequest& request : script) {
            outcomes.push_back(book.submitOrder(request).getEvents().toVector());
            expectSound(book, "scripted IOC");
        }
        return outcomes;
    };
    const std::vector<Events> first = run();
    expect(first == run(), "the same script should give the same outcomes on a fresh book");
    expect(first[2] == Events{OrderAccepted{3, Side::BUY, 101, 8}, TradeExecuted{3, 1, 101, 5}, OrderExpired{3, 3}},
           "scripted IOC: the IOC should take the first level and expire the rest");
    expect(first[4] == Events{OrderAccepted{5, Side::SELL, 98, 10}, TradeExecuted{5, 4, 99, 4}, OrderExpired{5, 6}},
           "scripted IOC: the IOC sell should take the bid and expire the rest");
    expect(first[7] == Events{OrderAccepted{7, Side::BUY, 102, 9}, TradeExecuted{7, 2, 102, 3}},
           "scripted IOC: the remaining ask should fill what the earlier IOC left");
}

}  // namespace

int main() {
    for (Side side : {Side::BUY, Side::SELL}) {
        testImmediateOrCancel(side);
    }
    testExpiredIdIsFree();
    testGoodTillCancelledRests();
    testScriptIsDeterministic();

    std::cout << "order behavior tests passed" << std::endl;
    return EXIT_SUCCESS;
}
