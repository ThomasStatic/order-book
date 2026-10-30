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
    static const auto& buyStops(const OrderBook& book) { return book.buyStops; }
    static const auto& sellStops(const OrderBook& book) { return book.sellStops; }
    static bool noneTriggered(const OrderBook& book) { return !book.triggeredStops || book.triggeredStops->empty(); }
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
using order_book::StopTriggered;
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

// Pending stops by stop price, each queue as (ID, sequence number), oldest first.
using StopState = std::vector<std::pair<Price, std::vector<std::pair<OrderId, SequenceNum>>>>;

struct BookState {
    std::vector<LevelState> bids;
    std::vector<LevelState> asks;
    StopState buyStops;
    StopState sellStops;
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

template <typename StopLevels>
StopState captureStops(const StopLevels& levels) {
    StopState captured;
    for (const auto& [price, level] : levels) {
        captured.emplace_back(price, std::vector<std::pair<OrderId, SequenceNum>>{});
        for (const auto& stop : level.stops) {
            captured.back().second.emplace_back(stop.request.id, stop.sequenceNum);
        }
    }
    return captured;
}

BookState capture(const OrderBook& book) {
    std::vector<OrderId> index;
    for (const auto& entry : Access::index(book)) {
        index.push_back(entry.first);
    }
    std::sort(index.begin(), index.end());
    return {captureLevels(Access::bids(book)), captureLevels(Access::asks(book)), captureStops(Access::buyStops(book)),
            captureStops(Access::sellStops(book)), index, Access::nextSequenceNum(book), Access::nextTradeId(book)};
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

OrderRequest stopOrder(OrderId id, Side side, Price stopPrice, unsigned int quantity,
                       TimeInForce timeInForce = TimeInForce::IOC) {
    OrderRequest request{id, side, 0, quantity, OrderType::STOP, timeInForce};
    request.stopPrice = stopPrice;
    return request;
}

OrderRequest stopLimit(OrderId id, Side side, Price stopPrice, Price limitPrice, unsigned int quantity,
                       TimeInForce timeInForce = TimeInForce::GTC) {
    OrderRequest request{id, side, limitPrice, quantity, OrderType::STOP_LIMIT, timeInForce};
    request.stopPrice = stopPrice;
    return request;
}

// A book whose last trade was at 100: a sell at 100 filled by a buy.
OrderBook tradedAt100() {
    OrderBook book;
    book.submitOrder(Order(90, 100, 1, 1, Side::SELL));
    book.submitOrder(Order(91, 100, 1, 1, Side::BUY));
    expect(book.getLastTradePrice() == Price{100}, "the setup should trade at 100");
    return book;
}

// A stop that is not past its trigger pends: its result holds only its acceptance (at its
// limit price, or 0 for a STOP), it consumes a sequence number, and it is no resting order.
void testPendingStops(Side side) {
    const std::string context = "pending stop " + sideName(side);
    const Price beyond = side == Side::BUY ? 110 : 90;  // not yet triggered by a trade at 100
    OrderBook book = tradedAt100();
    book.submitOrder(Order(1, side == Side::BUY ? 95 : 105, 4, 1, side));
    const BookState before = capture(book);
    const order_book::BookSnapshot snapshot = book.snapshot();

    const OrderRequest market = stopOrder(20, side, beyond, 5, TimeInForce::FOK);
    expectAcceptedWith(book.submitOrder(market), Events{OrderAccepted{20, side, 0, 5}}, 5, FillStatus::NONE, 0,
                       context + " STOP");
    const OrderRequest limited = stopLimit(21, side, beyond, beyond, 6);
    expectAcceptedWith(book.submitOrder(limited), Events{OrderAccepted{21, side, beyond, 6}}, 6, FillStatus::NONE, 0,
                       context + " STOP_LIMIT");
    OrderRequest owned = stopLimit(22, side, beyond, side == Side::BUY ? 120 : 80, 7, TimeInForce::IOC);
    owned.owner = 3;
    owned.selfTradePrevention = SelfTradePrevention::CANCEL_BOTH;
    book.submitOrder(owned);

    expect(book.pendingStopCount() == 3 && book.activeOrderCount() == 1, context + ": stops pend, and do not rest");
    expect(book.findPendingStop(20) == market && book.findPendingStop(21) == limited && book.findPendingStop(22) == owned,
           context + ": a pending stop should be found as the request accepted, every field included");
    expect(!book.findPendingStop(1) && !book.findPendingStop(99), context + ": only a pending stop is found");
    const order_book::BookSnapshot after = book.snapshot();
    expect(after.totalActiveVisibleOrders == snapshot.totalActiveVisibleOrders && after.bids.size() == snapshot.bids.size() &&
               after.asks.size() == snapshot.asks.size(),
           context + ": pending stops are in no snapshot");
    expect(Access::nextSequenceNum(book) == before.nextSequenceNum + 3 && Access::nextTradeId(book) == before.nextTradeId,
           context + ": each stop should consume one sequence number and no trade ID");
    expectSound(book, context);

    // Cancelling a pending stop reports its whole quantity and moves nothing else.
    BookState withStops = capture(book);
    const order_book::CancellationResult cancelled = book.cancelOrder(21);
    expect(cancelled.isSuccess() && cancelled.getEvents() == std::vector<BookEvent>{order_book::OrderCancelled{21, 6}},
           context + ": cancelling a pending stop should report its whole quantity");
    auto& stops = side == Side::BUY ? withStops.buyStops : withStops.sellStops;
    auto& queue = stops.front().second;
    queue.erase(queue.begin() + 1);
    withStops.index.clear();
    BookState now = capture(book);
    now.index.clear();
    expect(now == withStops && book.pendingStopCount() == 2, context + ": cancelling a stop should move nothing else");
    expect(book.cancelOrder(21).getRejectionReason() == order_book::RejectionReason::ORDER_NOT_FOUND,
           context + ": a cancelled stop is no longer live");
    expect(book.submitOrder(OrderRequest{21, side, side == Side::BUY ? Price{94} : Price{106}, 1}).getStatus() == SubmissionStatus::ACCEPTED,
           context + ": a cancelled stop's ID should be free");
    expect(book.cancelOrder(20).isSuccess() && book.cancelOrder(22).isSuccess() && book.pendingStopCount() == 0,
           context + ": every stop should cancel");
    expect(Access::buyStops(book).empty() && Access::sellStops(book).empty(), context + ": emptied stop levels are erased");
    expectSound(book, context + " after cancels");
}

// A stop already past its trigger on arrival (a buy stop at or below the last trade price, a
// sell stop at or above it) is accepted and wholly expired; before the first trade, none is.
void testStopsPastTheirTrigger(Side side) {
    const std::string context = "stop past its trigger " + sideName(side);
    for (const Price stopPrice : {Price{100}, side == Side::BUY ? Price{99} : Price{101}}) {
        OrderBook book = tradedAt100();
        BookState before = capture(book);
        expectAcceptedWith(book.submitOrder(stopOrder(20, side, stopPrice, 5)),
                           Events{OrderAccepted{20, side, 0, 5}, OrderExpired{20, 5}}, 5, FillStatus::NONE, 0,
                           context + " STOP at " + std::to_string(stopPrice));
        expectAcceptedWith(book.submitOrder(stopLimit(21, side, stopPrice, 100, 6)),
                           Events{OrderAccepted{21, side, 100, 6}, OrderExpired{21, 6}}, 6, FillStatus::NONE, 0,
                           context + " STOP_LIMIT at " + std::to_string(stopPrice));
        before.nextSequenceNum += 2;
        expect(capture(book) == before && book.pendingStopCount() == 0,
               context + ": only two sequence numbers should be consumed");
    }
    OrderBook fresh;
    expectAcceptedWith(fresh.submitOrder(stopOrder(20, side, 100, 5)), Events{OrderAccepted{20, side, 0, 5}}, 5,
                       FillStatus::NONE, 0, context + " before the first trade");
    expect(fresh.pendingStopCount() == 1, context + ": before the first trade every stop should pend");
}

// The last trade price is set by every trade and by nothing else, and travels with the book.
void testLastTradePrice() {
    OrderBook book;
    expect(!book.getLastTradePrice(), "no last trade price before the first trade");
    book.addOrder(Order(1, 100, 5, 1, Side::SELL));
    book.addOrder(Order(2, 99, 5, 1, Side::BUY));
    expect(!book.getLastTradePrice(), "addOrder sets no last trade price");
    book.submitOrder(OrderRequest{3, Side::BUY, 101, 2});
    expect(book.getLastTradePrice() == Price{100}, "a trade sets the last trade price");
    book.cancelOrder(2);
    book.submitOrder(OrderRequest{4, Side::SELL, 100, 1, OrderType::LIMIT, TimeInForce::IOC});
    expect(book.getLastTradePrice() == Price{100}, "an expiry or a cancel leaves it");
    book.cancelOrder(1);
    book.submitOrder(owned(OrderRequest{5, Side::SELL, 200, 1}, 9));
    book.submitOrder(owned(OrderRequest{6, Side::BUY, 200, 1}, 9));
    expect(book.findActiveOrder(6).getRemainingQuantity() == 1 && book.getLastTradePrice() == Price{100},
           "a prevented self-trade sets no last trade price");
    book.submitOrder(stopOrder(7, Side::SELL, 50, 1));

    OrderBook moved(std::move(book));
    expect(moved.getLastTradePrice() == Price{100} && moved.pendingStopCount() == 1 && moved.findPendingStop(7),
           "moving a book carries its last trade price and pending stops");
    OrderBook assigned;
    assigned.submitOrder(stopOrder(8, Side::BUY, 300, 1));
    assigned = std::move(moved);
    expect(assigned.getLastTradePrice() == Price{100} && assigned.pendingStopCount() == 1 && !assigned.findPendingStop(8),
           "move-assigning a book replaces its last trade price and pending stops");
    expect(assigned.cancelOrder(7).isSuccess(), "a moved book's pending stop should cancel");
    expectSound(assigned, "moved book");
}

// A trade at or through a pending stop's stop price triggers it. It runs after the order that
// traded has finished, as its market or limit order, with a fresh sequence number, and its
// events (opened by StopTriggered) follow in the same result.
void testTriggeredStops() {
    {
        // A sell stop at 99, triggered by a trade at 99: it becomes a market order and sells
        // into the bids that remain, after the submitting sell has finished.
        OrderBook book;
        book.submitOrder(Order(1, 99, 2, 1, Side::BUY));
        book.submitOrder(Order(2, 98, 3, 1, Side::BUY));
        book.submitOrder(stopOrder(10, Side::SELL, 99, 4));
        const SubmissionResult result = book.submitOrder(OrderRequest{20, Side::SELL, 99, 5});
        expectAcceptedWith(result,
                           Events{OrderAccepted{20, Side::SELL, 99, 5}, TradeExecuted{20, 1, 99, 2},
                                  StopTriggered{10, 99, 99}, TradeExecuted{10, 2, 98, 3}, OrderExpired{10, 1}},
                           3, FillStatus::PARTIAL, 2, "triggered sell stop");
        expect(book.findActiveOrder(20).getRemainingQuantity() == 3 && book.pendingStopCount() == 0 &&
                   book.getLastTradePrice() == Price{98},
               "triggered sell stop: the submitter's remainder rests first, then the stop runs");
        expectSound(book, "triggered sell stop");
    }
    {
        // One trade triggers a whole band, nearest stop price first, then acceptance order;
        // the trade price that triggered each is reported.
        OrderBook book;
        book.submitOrder(Order(1, 100, 1, 1, Side::BUY));  // last trade at 100 after the next line
        book.submitOrder(OrderRequest{2, Side::SELL, 100, 1});
        book.submitOrder(Order(3, 97, 9, 1, Side::BUY));
        book.submitOrder(stopOrder(10, Side::SELL, 98, 1));
        book.submitOrder(stopOrder(11, Side::SELL, 99, 1));
        book.submitOrder(stopOrder(12, Side::SELL, 98, 1));
        book.submitOrder(stopOrder(13, Side::SELL, 96, 1));  // below the trade: stays pending
        const SubmissionResult result = book.submitOrder(OrderRequest{20, Side::SELL, 97, 1});
        expect(result.getEvents() == Events{OrderAccepted{20, Side::SELL, 97, 1}, TradeExecuted{20, 3, 97, 1},
                                            StopTriggered{11, 99, 97}, TradeExecuted{11, 3, 97, 1},
                                            StopTriggered{10, 98, 97}, TradeExecuted{10, 3, 97, 1},
                                            StopTriggered{12, 98, 97}, TradeExecuted{12, 3, 97, 1}},
               "a band of stops runs nearest first, then in acceptance order");
        expect(result.getGeneratedTrades().size() == 4 && result.getRemainingQuantity() == 0 &&
                   result.getFillStatus() == FillStatus::FILLED,
               "the result's trades include the cascade's; its fill describes the submitted order");
        expect(book.pendingStopCount() == 1 && book.findPendingStop(13), "a stop beyond the trade stays pending");
        expectSound(book, "band of stops");
    }
    {
        // Breadth first: a stop triggered by a triggered stop's trade runs after every stop
        // already triggered. Buy stops 10 (at 101) and 11 (at 101) trigger on the submitter's
        // trade; 10's trade at 103 triggers 12 (at 103), which runs after 11.
        OrderBook book;
        book.submitOrder(Order(1, 101, 1, 1, Side::SELL));
        book.submitOrder(Order(2, 103, 1, 1, Side::SELL));
        book.submitOrder(Order(3, 104, 2, 1, Side::SELL));
        book.submitOrder(stopOrder(10, Side::BUY, 101, 1));
        book.submitOrder(stopOrder(11, Side::BUY, 101, 1));
        book.submitOrder(stopOrder(12, Side::BUY, 103, 1));
        const SubmissionResult result = book.submitOrder(OrderRequest{20, Side::BUY, 101, 1});
        expect(result.getEvents() == Events{OrderAccepted{20, Side::BUY, 101, 1}, TradeExecuted{20, 1, 101, 1},
                                            StopTriggered{10, 101, 101}, TradeExecuted{10, 2, 103, 1},
                                            StopTriggered{11, 101, 101}, TradeExecuted{11, 3, 104, 1},
                                            StopTriggered{12, 103, 103}, TradeExecuted{12, 3, 104, 1}},
               "a cascade runs breadth first");
        expectSound(book, "breadth-first cascade");
    }
    {
        // A triggered stop-limit takes a fresh sequence number: resting, it queues behind every
        // order accepted while it was pending. It may also trade with the submitter's remainder.
        OrderBook book;
        book.submitOrder(stopLimit(10, Side::SELL, 100, 101, 3));
        book.submitOrder(Order(1, 101, 1, 1, Side::SELL));  // accepted after the stop
        book.submitOrder(Order(2, 100, 1, 1, Side::BUY));
        const SubmissionResult result = book.submitOrder(OrderRequest{20, Side::SELL, 100, 1});
        expect(result.getEvents() == Events{OrderAccepted{20, Side::SELL, 100, 1}, TradeExecuted{20, 2, 100, 1},
                                            StopTriggered{10, 100, 100}},
               "a stop-limit that cannot trade at once rests");
        const auto& asks = Access::asks(book);
        expect(asks.at(101).getOrderCount() == 2 && Access::orders(asks.at(101)).back().getOrderId() == 10,
               "a triggered stop-limit should queue behind orders accepted while it was pending");
        const SequenceNum restedAt = book.findActiveOrder(10).getSequenceNum();
        expect(restedAt > book.findActiveOrder(1).getSequenceNum(), "a triggered stop should take a fresh number");
        // The submitting buy's remainder rests first, so a triggered sell stop can trade with it.
        OrderBook other;
        other.submitOrder(Order(1, 100, 1, 1, Side::SELL));
        other.submitOrder(stopOrder(10, Side::SELL, 100, 2));
        const SubmissionResult crossed = other.submitOrder(OrderRequest{20, Side::BUY, 100, 3});
        expect(crossed.getEvents() == Events{OrderAccepted{20, Side::BUY, 100, 3}, TradeExecuted{20, 1, 100, 1},
                                             StopTriggered{10, 100, 100}, TradeExecuted{10, 20, 100, 2}},
               "a triggered stop should trade with the submitter's freshly rested remainder");
        expect(crossed.getRemainingQuantity() == 2 && crossed.getFillStatus() == FillStatus::PARTIAL &&
                   other.activeOrderCount() == 0,
               "the submitter's remaining quantity is as at the end of its own match");
        expectSound(other, "stop against the submitter's remainder");
    }
    {
        // A FOK stop-limit walks the book as it stands at trigger time; unfillable, it expires.
        // Self-trade prevention applies with the stop's own owner and mode.
        OrderBook book;
        book.submitOrder(Order(1, 100, 1, 1, Side::SELL));
        book.submitOrder(owned(OrderRequest{2, Side::SELL, 101, 2}, 5));
        book.submitOrder(Order(3, 101, 1, 1, Side::SELL));
        book.submitOrder(stopLimit(10, Side::BUY, 100, 101, 9, TimeInForce::FOK));
        OrderRequest ownedStop = stopOrder(11, Side::BUY, 100, 2);
        ownedStop.owner = 5;
        book.submitOrder(ownedStop);
        const SubmissionResult result = book.submitOrder(OrderRequest{20, Side::BUY, 100, 1});
        expect(result.getEvents() == Events{OrderAccepted{20, Side::BUY, 100, 1}, TradeExecuted{20, 1, 100, 1},
                                            StopTriggered{10, 100, 100}, OrderExpired{10, 9}, StopTriggered{11, 100, 100},
                                            SelfTradePrevented{11, 2, 101, 2}, TradeExecuted{11, 3, 101, 1},
                                            OrderExpired{11, 1}},
               "a FOK stop walks the book at trigger time, and STP runs with the stop's own owner");
        expectSound(book, "FOK and owned stops");
    }
    {
        // submitOrder(Order) trades trigger stops too.
        OrderBook book;
        book.submitOrder(Order(1, 100, 1, 1, Side::SELL));
        book.submitOrder(Order(2, 102, 1, 1, Side::SELL));
        book.submitOrder(stopOrder(10, Side::BUY, 100, 1));
        const SubmissionResult result = book.submitOrder(Order(20, 100, 1, 1, Side::BUY));
        expect(result.getEvents() == Events{OrderAccepted{20, Side::BUY, 100, 1}, TradeExecuted{20, 1, 100, 1},
                                            StopTriggered{10, 100, 100}, TradeExecuted{10, 2, 102, 1}},
               "a v1 submission's trades trigger stops");
        expect(Access::noneTriggered(book), "the triggered queue is empty between commands");
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
        testPendingStops(side);
        testStopsPastTheirTrigger(side);
    }
    testLastTradePrice();
    testTriggeredStops();
    testExpiredIdIsFree();
    testGoodTillCancelledRests();
    testScriptIsDeterministic();

    std::cout << "order behavior tests passed" << std::endl;
    return EXIT_SUCCESS;
}
