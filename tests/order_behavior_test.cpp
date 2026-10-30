#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <tuple>
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
    static OwnerId owner(const OrderBook& book, OrderId id) { return book.orderIndex.find(id)->owner; }
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
using order_book::OwnerId;
using order_book::SelfTradePrevented;
using order_book::SelfTradePrevention;
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

OrderRequest fok(OrderId id, Side side, Price price, unsigned int quantity) {
    return OrderRequest{id, side, price, quantity, OrderType::LIMIT, TimeInForce::FOK};
}

// FOK fills its whole quantity at once, or makes no trades and wholly expires, leaving the book
// untouched but for the sequence number it consumes.
void testFillOrKill(Side side) {
    const std::string context = "FOK " + sideName(side);
    {
        // Fills across both levels, the last only in part: a normal match, nothing expires.
        OrderBook book = seededBook(side);
        const SubmissionResult result = book.submitOrder(fok(9, side, priceAway(side, 1), 10));
        expectAcceptedWith(result,
                           Events{OrderAccepted{9, side, priceAway(side, 1), 10},
                                  TradeExecuted{9, 1, priceAway(side, 0), 3}, TradeExecuted{9, 2, priceAway(side, 0), 4},
                                  TradeExecuted{9, 3, priceAway(side, 1), 3}},
                           0, FillStatus::FILLED, 3, context + " fillable");
        expect(book.findActiveOrder(3).getRemainingQuantity() == 2 && book.activeOrderCount() == 1,
               context + " fillable: the last maker should keep what was left");
        expectSound(book, context + " fillable");
    }
    {
        // Exactly everything within its limit.
        OrderBook book = seededBook(side);
        const SubmissionResult result = book.submitOrder(fok(9, side, priceAway(side, 0), 7));
        expectAcceptedWith(result,
                           Events{OrderAccepted{9, side, priceAway(side, 0), 7},
                                  TradeExecuted{9, 1, priceAway(side, 0), 3}, TradeExecuted{9, 2, priceAway(side, 0), 4}},
                           0, FillStatus::FILLED, 2, context + " exactly fillable");
        expectSound(book, context + " exactly fillable");
    }
    // Not fillable: one short within its limit, more beyond its limit, nothing crossing, or an
    // empty side. Accepted, then wholly expired, with no trade ID consumed.
    const std::pair<OrderRequest, const char*> unfillable[] = {
        {fok(9, side, priceAway(side, 1), 13), "one short"},
        {fok(9, side, priceAway(side, 0), 8), "liquidity beyond its limit"},
        {fok(9, side, priceAway(side, -1), 1), "not crossing"},
    };
    for (const auto& [request, what] : unfillable) {
        OrderBook book = seededBook(side);
        BookState before = capture(book);
        const SubmissionResult result = book.submitOrder(request);
        expectAcceptedWith(result,
                           Events{OrderAccepted{9, side, request.price, request.quantity}, OrderExpired{9, request.quantity}},
                           request.quantity, FillStatus::NONE, 0, context + " " + what);
        ++before.nextSequenceNum;
        expect(capture(book) == before, context + " " + what + ": only the sequence number should change");
        expectSound(book, context + " " + what);
    }
    OrderBook empty;
    expectAcceptedWith(empty.submitOrder(fok(9, side, 100, 4)), Events{OrderAccepted{9, side, 100, 4}, OrderExpired{9, 4}},
                       4, FillStatus::NONE, 0, context + " empty side");
    expect(Access::nextSequenceNum(empty) == 2 && Access::nextTradeId(empty) == 1 && empty.activeOrderCount() == 0,
           context + " empty side: should consume a sequence number and no trade ID");
}

OrderRequest market(OrderId id, Side side, unsigned int quantity, TimeInForce timeInForce = TimeInForce::IOC) {
    return OrderRequest{id, side, 0, quantity, OrderType::MARKET, timeInForce};
}

// A market order has no price protection: it trades at each resting price, as far through the
// opposite side as its quantity takes it, and expires whatever is left. Its acceptance reports
// price 0.
void testMarketOrders(Side side) {
    const std::string context = "market " + sideName(side);
    OrderBook book = seededBook(side);
    // A far level that no limit near the touch would reach.
    book.submitOrder(Order(4, side == Side::BUY ? 4000000000u : 1u, 2, 1, opposite(side)));
    const SubmissionResult sweep = book.submitOrder(market(9, side, 20));
    expectAcceptedWith(sweep,
                       Events{OrderAccepted{9, side, 0, 20}, TradeExecuted{9, 1, priceAway(side, 0), 3},
                              TradeExecuted{9, 2, priceAway(side, 0), 4}, TradeExecuted{9, 3, priceAway(side, 1), 5},
                              TradeExecuted{9, 4, side == Side::BUY ? 4000000000u : 1u, 2}, OrderExpired{9, 6}},
                       6, FillStatus::PARTIAL, 4, context + " sweep");
    expect(book.activeOrderCount() == 0 && !book.hasBids() && !book.hasAsks(),
           context + " sweep: it should empty the opposite side and rest nothing");
    expectSound(book, context + " sweep");

    OrderBook partial = seededBook(side);
    expectAcceptedWith(partial.submitOrder(market(9, side, 5)),
                       Events{OrderAccepted{9, side, 0, 5}, TradeExecuted{9, 1, priceAway(side, 0), 3},
                              TradeExecuted{9, 2, priceAway(side, 0), 2}},
                       0, FillStatus::FILLED, 2, context + " filled");

    OrderBook empty;
    expectAcceptedWith(empty.submitOrder(market(9, side, 5)), Events{OrderAccepted{9, side, 0, 5}, OrderExpired{9, 5}}, 5,
                       FillStatus::NONE, 0, context + " empty side");
    expect(Access::nextSequenceNum(empty) == 2 && Access::nextTradeId(empty) == 1, context + " empty side: one number");

    // Market FOK: all of it, or none.
    OrderBook fillable = seededBook(side);
    expectAcceptedWith(fillable.submitOrder(market(9, side, 12, TimeInForce::FOK)),
                       Events{OrderAccepted{9, side, 0, 12}, TradeExecuted{9, 1, priceAway(side, 0), 3},
                              TradeExecuted{9, 2, priceAway(side, 0), 4}, TradeExecuted{9, 3, priceAway(side, 1), 5}},
                       0, FillStatus::FILLED, 3, context + " FOK fillable");
    OrderBook unfillable = seededBook(side);
    BookState before = capture(unfillable);
    expectAcceptedWith(unfillable.submitOrder(market(9, side, 13, TimeInForce::FOK)),
                       Events{OrderAccepted{9, side, 0, 13}, OrderExpired{9, 13}}, 13, FillStatus::NONE, 0,
                       context + " FOK unfillable");
    ++before.nextSequenceNum;
    expect(capture(unfillable) == before, context + " FOK unfillable: only the sequence number should change");
}

OrderRequest postOnly(OrderId id, Side side, Price price, unsigned int quantity) {
    OrderRequest request{id, side, price, quantity};
    request.postOnly = true;
    return request;
}

// A post-only order that would take (at or through the best opposing price) is accepted and
// wholly expired, never repriced; otherwise it rests as an ordinary limit order.
void testPostOnly(Side side) {
    const std::string context = "post-only " + sideName(side);
    for (int ticks : {0, 1, 5}) {
        OrderBook book = seededBook(side);
        BookState before = capture(book);
        const SubmissionResult result = book.submitOrder(postOnly(9, side, priceAway(side, ticks), 6));
        expectAcceptedWith(result, Events{OrderAccepted{9, side, priceAway(side, ticks), 6}, OrderExpired{9, 6}}, 6,
                           FillStatus::NONE, 0, context + " would take " + std::to_string(ticks));
        ++before.nextSequenceNum;
        expect(capture(book) == before, context + " would take: only the sequence number should change");
        expectSound(book, context + " would take");
    }
    {
        // Rests behind the touch, exactly as a GTC limit order would, with the next number.
        OrderBook book = seededBook(side);
        book.submitOrder(Order(5, priceAway(side, -1), 2, 1, side));
        const SequenceNum sequence = Access::nextSequenceNum(book);
        const SubmissionResult result = book.submitOrder(postOnly(9, side, priceAway(side, -1), 6));
        expectAcceptedWith(result, Events{OrderAccepted{9, side, priceAway(side, -1), 6}}, 6, FillStatus::NONE, 0,
                           context + " rests");
        const Order rested = book.findActiveOrder(9);
        expect(rested.getSequenceNum() == sequence && Access::nextSequenceNum(book) == sequence + 1,
               context + " rests: it should take the next sequence number");
        expectSound(book, context + " rests");
    }
    {
        // An empty opposite side never takes.
        OrderBook book;
        expectAcceptedWith(book.submitOrder(postOnly(9, side, 100, 6)), Events{OrderAccepted{9, side, 100, 6}}, 6,
                           FillStatus::NONE, 0, context + " empty side");
        expect(book.activeOrderCount() == 1, context + " empty side: it should rest");
    }
}

OrderRequest owned(OrderRequest request, OwnerId owner, SelfTradePrevention mode = SelfTradePrevention::CANCEL_RESTING) {
    request.owner = owner;
    request.selfTradePrevention = mode;
    return request;
}

// Asks: #1 (owner 7, 3) and #2 (owner 8, 4) at 100, #3 (owner 7, 5) at 101, #4 (no owner, 6) at
// 101; mirrored for an incoming sell.
OrderBook ownedBook(Side incomingSide) {
    const Side restingSide = opposite(incomingSide);
    OrderBook book;
    book.submitOrder(owned(OrderRequest{1, restingSide, priceAway(incomingSide, 0), 3}, 7));
    book.submitOrder(owned(OrderRequest{2, restingSide, priceAway(incomingSide, 0), 4}, 8));
    book.submitOrder(owned(OrderRequest{3, restingSide, priceAway(incomingSide, 1), 5}, 7));
    book.submitOrder(OrderRequest{4, restingSide, priceAway(incomingSide, 1), 6});
    return book;
}

// The incoming order's mode decides which of two same-owner orders gives way; a prevented
// match is never a trade, consumes no trade ID, and is reported in execution order.
void testSelfTradePrevention(Side side) {
    const std::string context = "STP " + sideName(side);
    const Price near = priceAway(side, 0);
    const Price far = priceAway(side, 1);
    {
        OrderBook book = ownedBook(side);
        expect(Access::owner(book, 1) == 7 && Access::owner(book, 2) == 8 && Access::owner(book, 4) == 0,
               context + ": a resting order should keep its request's owner");
        // CANCEL_RESTING: each own order is removed whole, and matching goes on; the GTC
        // remainder rests.
        const SubmissionResult result =
            book.submitOrder(owned(OrderRequest{9, side, far, 20}, 7, SelfTradePrevention::CANCEL_RESTING));
        expectAcceptedWith(result,
                           Events{OrderAccepted{9, side, far, 20}, SelfTradePrevented{9, 1, near, 3},
                                  TradeExecuted{9, 2, near, 4}, SelfTradePrevented{9, 3, far, 5},
                                  TradeExecuted{9, 4, far, 6}},
                           10, FillStatus::PARTIAL, 2, context + " cancel resting");
        expect(book.activeOrderCount() == 1 && book.findActiveOrder(9).getRemainingQuantity() == 10 &&
                   Access::owner(book, 9) == 7,
               context + " cancel resting: only the incoming remainder should rest, with its owner");
        expect(Access::nextTradeId(book) == 3, context + " cancel resting: a prevention should consume no trade ID");
        expectSound(book, context + " cancel resting");
        // The removed orders' IDs are free again.
        expect(book.submitOrder(OrderRequest{1, side, priceAway(side, -5), 1}).getStatus() == SubmissionStatus::ACCEPTED,
               context + " cancel resting: a removed order's ID should be free");
    }
    {
        // CANCEL_INCOMING: the own order stays, matching stops, and even a GTC remainder expires.
        OrderBook book = ownedBook(side);
        const SubmissionResult result = book.submitOrder(
            owned(OrderRequest{9, side, far, 20}, 8, SelfTradePrevention::CANCEL_INCOMING));
        expectAcceptedWith(result,
                           Events{OrderAccepted{9, side, far, 20}, TradeExecuted{9, 1, near, 3},
                                  SelfTradePrevented{9, 2, near, 0}, OrderExpired{9, 17}},
                           17, FillStatus::PARTIAL, 1, context + " cancel incoming");
        expect(book.findActiveOrder(2).getRemainingQuantity() == 4 && book.activeOrderCount() == 3,
               context + " cancel incoming: the resting order should be untouched, and nothing rest");
        expectSound(book, context + " cancel incoming");
    }
    {
        // CANCEL_BOTH: the own order is removed, and the remainder expires.
        OrderBook book = ownedBook(side);
        const SubmissionResult result =
            book.submitOrder(owned(OrderRequest{9, side, far, 20}, 8, SelfTradePrevention::CANCEL_BOTH));
        expectAcceptedWith(result,
                           Events{OrderAccepted{9, side, far, 20}, TradeExecuted{9, 1, near, 3},
                                  SelfTradePrevented{9, 2, near, 4}, OrderExpired{9, 17}},
                           17, FillStatus::PARTIAL, 1, context + " cancel both");
        expect(book.activeOrderCount() == 2 && !Access::index(book).count(2), context + " cancel both: #2 should be gone");
        expectSound(book, context + " cancel both");
    }
    {
        // Owner 0 is never prevented, whatever the mode, and different owners trade freely.
        OrderBook book = ownedBook(side);
        const SubmissionResult result =
            book.submitOrder(owned(OrderRequest{9, side, far, 18}, 0, SelfTradePrevention::CANCEL_BOTH));
        expectAcceptedWith(result,
                           Events{OrderAccepted{9, side, far, 18}, TradeExecuted{9, 1, near, 3}, TradeExecuted{9, 2, near, 4},
                                  TradeExecuted{9, 3, far, 5}, TradeExecuted{9, 4, far, 6}},
                           0, FillStatus::FILLED, 4, context + " no owner");
        OrderBook other = ownedBook(side);
        expect(other.submitOrder(owned(OrderRequest{9, side, far, 18}, 5)).getGeneratedTrades().size() == 4,
               context + ": an owner no resting order has should trade with every order");
    }
    {
        // A market order with CANCEL_RESTING removes each own order it reaches.
        OrderBook book = ownedBook(side);
        const SubmissionResult result =
            book.submitOrder(owned(OrderRequest{9, side, 0, 30, OrderType::MARKET, TimeInForce::IOC}, 7));
        expectAcceptedWith(result,
                           Events{OrderAccepted{9, side, 0, 30}, SelfTradePrevented{9, 1, near, 3},
                                  TradeExecuted{9, 2, near, 4}, SelfTradePrevented{9, 3, far, 5},
                                  TradeExecuted{9, 4, far, 6}, OrderExpired{9, 20}},
                           20, FillStatus::PARTIAL, 2, context + " market");
        expect(book.activeOrderCount() == 0, context + " market: the side should be empty");
    }
    {
        // Post-only never matches, so it never runs STP: one that would take only its own order
        // still expires.
        OrderBook book = ownedBook(side);
        OrderRequest request = owned(OrderRequest{9, side, near, 2}, 7);
        request.postOnly = true;
        expectAcceptedWith(book.submitOrder(request), Events{OrderAccepted{9, side, near, 2}, OrderExpired{9, 2}}, 2,
                           FillStatus::NONE, 0, context + " post-only");
        expect(book.activeOrderCount() == 4, context + " post-only: nothing should be removed");
    }
    {
        // addOrder and submitOrder(Order) give owner 0.
        OrderBook book;
        book.addOrder(Order(1, priceAway(side, -3), 1, 1, side));
        book.submitOrder(Order(2, priceAway(side, -3), 1, 1, side));
        expect(Access::owner(book, 1) == 0 && Access::owner(book, 2) == 0, context + ": v1 commands give owner 0");
    }
}

// A FOK with an owner never counts a self-match toward filling. Under CANCEL_RESTING its walk
// skips its own orders (and removes them only if it fills); under the other modes it stops at
// the first one. An unfillable FOK cancels nothing and reports no prevention.
void testFillOrKillWithOwner(Side side) {
    const std::string context = "FOK with owner " + sideName(side);
    const Price near = priceAway(side, 0);
    const Price far = priceAway(side, 1);
    {
        OrderBook book = ownedBook(side);
        const SubmissionResult result = book.submitOrder(
            owned(OrderRequest{9, side, far, 10, OrderType::LIMIT, TimeInForce::FOK}, 7));
        expectAcceptedWith(result,
                           Events{OrderAccepted{9, side, far, 10}, SelfTradePrevented{9, 1, near, 3},
                                  TradeExecuted{9, 2, near, 4}, SelfTradePrevented{9, 3, far, 5},
                                  TradeExecuted{9, 4, far, 6}},
                           0, FillStatus::FILLED, 2, context + " cancel resting, fillable");
        expectSound(book, context + " cancel resting, fillable");
    }
    for (const auto& [mode, quantity, what] :
         {std::tuple{SelfTradePrevention::CANCEL_RESTING, 11u, "cancel resting, one short of the others"},
          std::tuple{SelfTradePrevention::CANCEL_INCOMING, 4u, "cancel incoming, blocked at its own order"},
          std::tuple{SelfTradePrevention::CANCEL_BOTH, 1u, "cancel both, blocked at its own order"}}) {
        OrderBook book = ownedBook(side);
        BookState before = capture(book);
        const SubmissionResult result =
            book.submitOrder(owned(OrderRequest{9, side, far, quantity, OrderType::LIMIT, TimeInForce::FOK}, 7, mode));
        expectAcceptedWith(result, Events{OrderAccepted{9, side, far, quantity}, OrderExpired{9, quantity}}, quantity,
                           FillStatus::NONE, 0, context + " " + what);
        ++before.nextSequenceNum;
        expect(capture(book) == before, context + " " + what + ": nothing should be cancelled");
    }
    {
        // CANCEL_INCOMING fills when its quantity comes before its own order.
        OrderBook book = ownedBook(side);
        const SubmissionResult result = book.submitOrder(
            owned(OrderRequest{9, side, far, 3, OrderType::LIMIT, TimeInForce::FOK}, 8, SelfTradePrevention::CANCEL_INCOMING));
        expectAcceptedWith(result, Events{OrderAccepted{9, side, far, 3}, TradeExecuted{9, 1, near, 3}}, 0,
                           FillStatus::FILLED, 1, context + " cancel incoming, filled before its own order");
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
            fok(8, Side::SELL, 100, 7), fok(8, Side::SELL, 100, 6), {9, Side::SELL, 105, 2}, fok(10, Side::BUY, 105, 3),
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
    expect(first[8] == Events{OrderAccepted{8, Side::SELL, 100, 7}, OrderExpired{8, 7}},
           "scripted FOK: a sell for more than the bid holds should expire whole");
    expect(first[9] == Events{OrderAccepted{8, Side::SELL, 100, 6}, TradeExecuted{8, 7, 102, 6}},
           "scripted FOK: a sell for exactly the bid should fill");
    expect(first[11] == Events{OrderAccepted{10, Side::BUY, 105, 3}, OrderExpired{10, 3}},
           "scripted FOK: a buy for more than the ask holds should expire whole");
}

}  // namespace

int main() {
    for (Side side : {Side::BUY, Side::SELL}) {
        testImmediateOrCancel(side);
        testFillOrKill(side);
        testMarketOrders(side);
        testPostOnly(side);
        testSelfTradePrevention(side);
        testFillOrKillWithOwner(side);
    }
    testExpiredIdIsFree();
    testGoodTillCancelledRests();
    testScriptIsDeterministic();

    std::cout << "order behavior tests passed" << std::endl;
    return EXIT_SUCCESS;
}
