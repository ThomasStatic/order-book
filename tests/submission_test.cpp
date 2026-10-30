#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <list>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "order_book/order.hpp"
#include "order_book/order_book.hpp"
#include "order_book/order_request.hpp"
#include "order_book/submission_result.hpp"

namespace order_book {

// Befriended by OrderBook, PriceLevel, and Order so these tests can inspect internals and
// build malformed orders the Order constructor would refuse.
struct OrderBookTestAccess {
    static const auto& index(const OrderBook& book) { return book.orderIndex; }
    static const auto& bids(const OrderBook& book) { return book.bids; }
    static const auto& asks(const OrderBook& book) { return book.asks; }
    static TradeId nextTradeId(const OrderBook& book) { return book.nextTradeId; }
    static SequenceNum nextSequenceNum(const OrderBook& book) { return book.nextSequenceNum; }

    static const order_book::OrderQueue& orders(const PriceLevel& level) { return level.orders; }

    static void setOrderId(Order& order, OrderId orderId) { order.orderId = orderId; }
    static void setPrice(Order& order, Price price) { order.priceTick = price; }
    static void setQuantity(Order& order, unsigned int quantity) { order.initialQuantity = order.remainingQuantity = quantity; }
    static void setSide(Order& order, Side side) { order.side = side; }
    static void setOrderType(Order& order, OrderType type) { order.type = type; }
};

}  // namespace order_book

namespace {

using Access = order_book::OrderBookTestAccess;
using order_book::FillStatus;
using order_book::Order;
using order_book::OrderBook;
using order_book::OrderId;
using order_book::OrderType;
using order_book::Price;
using order_book::RejectionReason;
using order_book::SequenceNum;
using order_book::Side;
using order_book::SubmissionResult;
using order_book::SubmissionStatus;
using order_book::TradeId;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

// One index entry per resting order, pointing at it, no empty levels, aggregates matching
// their orders, and the active-order count equal to the number of resting orders, summed
// level by level.
template <typename Levels>
void expectSideConsistent(const OrderBook& book, const Levels& levels, Side side, std::size_t& restingOrders,
                          const std::string& context) {
    for (const auto& [price, level] : levels) {
        expect(!level.isEmpty(), context + ": no empty price level should remain in the book");

        unsigned int remainingSum = 0;
        for (const Order& order : Access::orders(level)) {
            ++restingOrders;
            remainingSum += order.getRemainingQuantity();
            expect(order.getRemainingQuantity() > 0, context + ": every resting order should have quantity left");
            auto indexItr = Access::index(book).find(order.getOrderId());
            expect(indexItr != Access::index(book).end(), context + ": every resting order should be indexed");
            expect(&*indexItr->second.itr == &order, context + ": an index entry should point at its order");
            expect(indexItr->second.tickPrice == price && indexItr->second.side == side,
                   context + ": an index entry should record the level price and side");
        }
        expect(level.getAggregateQuantity() == remainingSum,
               context + ": a level's aggregate quantity should equal its orders' remaining quantities");
    }
}

void expectBookConsistent(const OrderBook& book, const std::string& context) {
    std::size_t restingOrders = 0;
    expectSideConsistent(book, Access::bids(book), Side::BUY, restingOrders, context);
    expectSideConsistent(book, Access::asks(book), Side::SELL, restingOrders, context);
    expect(Access::index(book).size() == restingOrders,
           context + ": the index should hold exactly one entry per resting order");
    expect(book.activeOrderCount() == restingOrders,
           context + ": the active-order count should equal the number of resting orders");
    // The library's checker should agree with these hand-written checks.
    const order_book::InvariantResult invariants = book.checkInvariants();
    expect(invariants.isValid(),
           context + ": the book should pass its invariant check, got " + order_book::formatInvariantResult(invariants));
}

bool isIndexed(const OrderBook& book, OrderId orderId) {
    return Access::index(book).find(orderId) != Access::index(book).end();
}

Side opposite(Side side) {
    return side == Side::BUY ? Side::SELL : Side::BUY;
}

std::string sideName(Side side) {
    return side == Side::BUY ? "buy" : "sell";
}

// Resting liquidity sits at 100 and further from the incoming side (asks rise, bids
// fall), so an incoming limit at priceAway(side, n) crosses every resting level up to n.
Price priceAway(Side incomingSide, int ticksAway) {
    return static_cast<Price>(incomingSide == Side::BUY ? 100 + ticksAway : 100 - ticksAway);
}

// Everything a submission could disturb, including each order's address, so comparing two
// states also shows that no resting order was moved, reinserted, or reprioritised.
struct OrderState {
    OrderId id;
    SequenceNum sequenceNum;
    unsigned int initialQuantity;
    unsigned int remainingQuantity;
    FillStatus fillStatus;
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
    std::vector<std::pair<OrderId, const Order*>> index;
    std::size_t activeOrders;
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
            levelState.orders.push_back({order.getOrderId(), order.getSequenceNum(), order.getInitialQuantity(),
                                         order.getRemainingQuantity(), order.getFillStatus(), &order});
        }
        captured.push_back(levelState);
    }
    return captured;
}

BookState capture(const OrderBook& book) {
    std::vector<std::pair<OrderId, const Order*>> index;
    for (const auto& entry : Access::index(book)) {
        const auto& [orderId, owner, location] = entry;
        static_cast<void>(owner);
        index.emplace_back(orderId, &*location.itr);
    }
    std::sort(index.begin(), index.end());
    return {captureLevels(Access::bids(book)), captureLevels(Access::asks(book)), index, book.activeOrderCount(),
            Access::nextSequenceNum(book), Access::nextTradeId(book)};
}

// Bids at 99 and 98, asks at 101 and 102, two orders on each best level.
OrderBook restingBook() {
    OrderBook book;
    book.addOrder(Order(1, 99, 5, 1, Side::BUY));
    book.addOrder(Order(2, 99, 3, 1, Side::BUY));
    book.addOrder(Order(3, 98, 4, 1, Side::BUY));
    book.addOrder(Order(4, 101, 6, 1, Side::SELL));
    book.addOrder(Order(5, 101, 2, 1, Side::SELL));
    book.addOrder(Order(6, 102, 7, 1, Side::SELL));
    return book;
}

void expectRejected(const SubmissionResult& result, OrderId orderId, unsigned int quantity, RejectionReason reason,
                    const std::string& context) {
    expect(result.getStatus() == SubmissionStatus::REJECTED, context + ": the submission should be rejected");
    expect(result.getRejectionReason() == reason, context + ": unexpected rejection reason");
    expect(result.getSubmissionId() == orderId, context + ": the result should be identified by the incoming order id");
    expect(result.getGeneratedTrades().empty(), context + ": a rejected submission should generate no trades");
    expect(result.getRemainingQuantity() == quantity,
           context + ": a rejected submission should report its whole quantity as unexecuted");
    expect(result.getFillStatus() == FillStatus::NONE, context + ": a rejected submission should report no fill");
}

// Submits an order the book must reject and checks that nothing observable changed.
void expectRejectedUnchanged(OrderBook& book, const Order& incoming, RejectionReason reason,
                             const std::string& context) {
    const BookState before = capture(book);
    const SubmissionResult result = book.submitOrder(incoming);
    expectRejected(result, incoming.getOrderId(), incoming.getRemainingQuantity(), reason, context);
    expect(capture(book) == before, context + ": a rejected submission should leave the book exactly as it was");
    expectBookConsistent(book, context);
}

void expectAccepted(const SubmissionResult& result, OrderId orderId, std::size_t tradeCount,
                    unsigned int remainingQuantity, FillStatus fillStatus, const std::string& context) {
    expect(result.getStatus() == SubmissionStatus::ACCEPTED, context + ": the submission should be accepted");
    expect(result.getRejectionReason() == RejectionReason::NONE,
           context + ": an accepted submission should carry no rejection reason");
    expect(result.getSubmissionId() == orderId, context + ": the result should be identified by the incoming order id");
    expect(result.getGeneratedTrades().size() == tradeCount, context + ": unexpected number of trades");
    expect(result.getRemainingQuantity() == remainingQuantity, context + ": unexpected remaining quantity");
    expect(result.getFillStatus() == fillStatus, context + ": unexpected fill status");
}

void testFullyFilledIncomingIsNotIndexed(Side incomingSide) {
    const std::string context = "fully filled incoming " + sideName(incomingSide);
    const Side restingSide = opposite(incomingSide);
    OrderBook book;
    book.addOrder(Order(1, priceAway(incomingSide, 0), 4, 1, restingSide));
    book.addOrder(Order(2, priceAway(incomingSide, 0), 6, 1, restingSide));
    expect(book.activeOrderCount() == 2, context + ": two orders should be active before the submission");

    const SubmissionResult result = book.submitOrder(Order(3, priceAway(incomingSide, 0), 7, 1, incomingSide));

    expectAccepted(result, 3, 2, 0, FillStatus::FILLED, context);
    expect(!isIndexed(book, 3), context + ": a fully filled incoming order should never be indexed");
    expect(!isIndexed(book, 1) && isIndexed(book, 2), context + ": only the partly filled maker should stay active");
    expect(book.activeOrderCount() == 1, context + ": one maker should remain active");
    expectBookConsistent(book, context);
}

void testDuplicateActiveIdIsRejected() {
    const std::string context = "duplicate active id";
    for (OrderId restingId : {1u, 2u, 4u, 6u}) {
        const std::string idContext = context + " " + std::to_string(restingId);
        OrderBook book = restingBook();
        // Would sweep both ask levels (or both bid levels) if accepted.
        expectRejectedUnchanged(book, Order(restingId, 102, 20, 1, Side::BUY), RejectionReason::DUPLICATE_ORDER_ID,
                                idContext + " crossing buy");
        expectRejectedUnchanged(book, Order(restingId, 98, 20, 1, Side::SELL), RejectionReason::DUPLICATE_ORDER_ID,
                                idContext + " crossing sell");
        // Would open a new level inside the spread if accepted.
        expectRejectedUnchanged(book, Order(restingId, 100, 1, 1, Side::BUY), RejectionReason::DUPLICATE_ORDER_ID,
                                idContext + " passive buy");
    }
}

// An ID belongs to an order only while it rests: once cancelled it may be submitted again.
void testReusedCancelledIdIsAccepted() {
    const std::string context = "reused cancelled id";
    OrderBook book = restingBook();
    expect(book.cancelOrder(4).isSuccess(), context + ": cancelling the resting order should succeed");

    const SubmissionResult crossing = book.submitOrder(Order(4, 101, 1, 1, Side::BUY));
    expectAccepted(crossing, 4, 1, 0, FillStatus::FILLED, context + " crossing");
    expect(crossing.getGeneratedTrades()[0].getRestingId() == 5,
           context + ": the reused id should trade against the remaining order at 101");

    expect(book.cancelOrder(1).isSuccess(), context + ": cancelling the resting bid should succeed");
    const SubmissionResult resting = book.submitOrder(Order(1, 97, 3, 1, Side::BUY));
    expectAccepted(resting, 1, 0, 3, FillStatus::NONE, context + " resting");
    expect(isIndexed(book, 1) && book.findActiveOrder(1).getPriceTick() == 97,
           context + ": the reused id should rest at its new price");
    expectBookConsistent(book, context);
}

// A fully filled order, maker or taker, no longer rests, so its ID is free again.
void testReusedFilledIdIsAccepted() {
    const std::string context = "reused filled id";
    OrderBook book = restingBook();
    // Fills maker 4 completely; taker 7 fills completely too.
    expectAccepted(book.submitOrder(Order(7, 101, 6, 1, Side::BUY)), 7, 1, 0, FillStatus::FILLED, context);
    expect(!isIndexed(book, 4) && !isIndexed(book, 7), context + ": neither filled order should stay active");

    expectAccepted(book.submitOrder(Order(4, 100, 2, 1, Side::SELL)), 4, 0, 2, FillStatus::NONE,
                   context + " maker id");
    expectAccepted(book.submitOrder(Order(7, 99, 8, 1, Side::SELL)), 7, 2, 0, FillStatus::FILLED,
                   context + " taker id");
    expect(isIndexed(book, 4) && book.findActiveOrder(4).getPriceTick() == 100,
           context + ": the reused maker id should rest at its new price");
    expect(!isIndexed(book, 7), context + ": the reused taker id filled completely again");
    expectBookConsistent(book, context);
}

// A crossing order that would sweep both ask levels if accepted, so any leak of a
// malformed order into matching shows up as trades or a changed book.
Order crossingBuy(OrderId orderId = 9) {
    return Order(orderId, 102, 20, 1, Side::BUY);
}

// The Order constructor refuses all of these; the book refuses them again in case one is
// built some other way, before any matching or mutation.
void testMalformedOrdersAreRejected() {
    const std::string context = "malformed order";
    OrderBook book = restingBook();

    Order reservedId = crossingBuy();
    Access::setOrderId(reservedId, 0);
    expectRejectedUnchanged(book, reservedId, RejectionReason::INVALID_ORDER_ID, context + " reserved id 0");

    for (Side side : {Side::BUY, Side::SELL}) {
        // A zero-priced sell would cross every bid.
        Order zeroPrice(9, 100, 5, 1, side);
        Access::setPrice(zeroPrice, 0);
        expectRejectedUnchanged(book, zeroPrice, RejectionReason::INVALID_PRICE,
                                context + " zero price " + sideName(side));
    }

    Order zeroQuantity = crossingBuy();
    Access::setQuantity(zeroQuantity, 0);
    expectRejectedUnchanged(book, zeroQuantity, RejectionReason::INVALID_QUANTITY, context + " zero quantity");

    // Reachable without test access: filled to nothing, then its status reset.
    Order emptied = crossingBuy();
    emptied.fillQuantity(20);
    emptied.setFillStatus(FillStatus::NONE);
    expectRejectedUnchanged(book, emptied, RejectionReason::INVALID_QUANTITY, context + " no remaining quantity");

    for (int rawSide : {2, -1, 255}) {
        Order badSide = crossingBuy();
        Access::setSide(badSide, static_cast<Side>(rawSide));
        expectRejectedUnchanged(book, badSide, RejectionReason::INVALID_SIDE,
                                context + " side " + std::to_string(rawSide));
    }

    // An Order accepts only a type that can rest: MARKET is an enumerator, but cannot.
    Order marketOrder = crossingBuy();
    Access::setOrderType(marketOrder, OrderType::MARKET);
    expectRejectedUnchanged(book, marketOrder, RejectionReason::UNSUPPORTED_ORDER_TYPE, context + " market Order");
    Order badType = crossingBuy();
    Access::setOrderType(badType, static_cast<OrderType>(9));
    expectRejectedUnchanged(book, badType, RejectionReason::UNSUPPORTED_ORDER_TYPE, context + " unsupported type");

    // Reachable without test access: marked FILLED while quantity remains.
    Order markedFilled = crossingBuy();
    markedFilled.setFillStatus(FillStatus::FILLED);
    expectRejectedUnchanged(book, markedFilled, RejectionReason::ORDER_NOT_ACTIVE, context + " marked filled");

    // Malformed is reported ahead of duplicate when an order is both.
    Order malformedDuplicate = crossingBuy(4);
    Access::setPrice(malformedDuplicate, 0);
    expectRejectedUnchanged(book, malformedDuplicate, RejectionReason::INVALID_PRICE, context + " and duplicate");
}

void expectTradePrices(const SubmissionResult& result, std::initializer_list<Price> prices,
                       const std::string& context) {
    expect(result.getGeneratedTrades().size() == prices.size(), context + ": unexpected number of trades");
    std::size_t i = 0;
    for (Price price : prices) {
        expect(result.getGeneratedTrades()[i++].getExecutionPrice() == price,
               context + ": trades should execute at the resting prices, best first");
    }
}

void expectRestingRemainder(const OrderBook& book, OrderId orderId, Price price, unsigned int initial,
                            unsigned int remaining, const std::string& context) {
    expect(isIndexed(book, orderId), context + ": the resting remainder should be indexed");
    const Order rested = book.findActiveOrder(orderId);
    expect(rested.getPriceTick() == price, context + ": the remainder should rest at its own limit price");
    expect(rested.getInitialQuantity() == initial && rested.getRemainingQuantity() == remaining,
           context + ": the remainder should keep its initial quantity and rest only what is left");
    expect(rested.getFillStatus() == FillStatus::PARTIAL, context + ": the remainder should be partially filled");
}

void testPartiallyFilledIncomingRestsIndexed() {
    {
        const std::string context = "partially filled incoming buy";
        OrderBook book = restingBook();
        const SubmissionResult result = book.submitOrder(Order(9, 101, 10, 1, Side::BUY));
        expectAccepted(result, 9, 2, 2, FillStatus::PARTIAL, context);
        expectRestingRemainder(book, 9, 101, 10, 2, context);
        expect(book.getBestBid().getOrderId() == 9, context + ": the remainder should be the new best bid");
        expect(book.activeOrderCount() == 5, context + ": two makers filled and the remainder joined");
        expectBookConsistent(book, context);
    }
    {
        const std::string context = "partially filled incoming sell";
        OrderBook book = restingBook();
        const SubmissionResult result = book.submitOrder(Order(9, 99, 10, 1, Side::SELL));
        expectAccepted(result, 9, 2, 2, FillStatus::PARTIAL, context);
        expectRestingRemainder(book, 9, 99, 10, 2, context);
        expect(book.getBestAsk().getOrderId() == 9, context + ": the remainder should be the new best ask");
        expect(book.activeOrderCount() == 5, context + ": two makers filled and the remainder joined");
        expectBookConsistent(book, context);
    }
}

void testMultipleLevelSweep() {
    {
        const std::string context = "buy sweeping both ask levels";
        OrderBook book = restingBook();
        const SubmissionResult result = book.submitOrder(Order(9, 102, 20, 1, Side::BUY));
        expectAccepted(result, 9, 3, 5, FillStatus::PARTIAL, context);
        expectTradePrices(result, {101, 101, 102}, context);
        expect(!book.hasAsks(), context + ": every ask should be consumed");
        expectRestingRemainder(book, 9, 102, 20, 5, context);
        expect(book.activeOrderCount() == 4, context + ": three bids and the remainder should be active");
        expectBookConsistent(book, context);
    }
    {
        const std::string context = "sell sweeping both bid levels";
        OrderBook book = restingBook();
        const SubmissionResult result = book.submitOrder(Order(9, 98, 20, 1, Side::SELL));
        expectAccepted(result, 9, 3, 8, FillStatus::PARTIAL, context);
        expectTradePrices(result, {99, 99, 98}, context);
        expect(!book.hasBids(), context + ": every bid should be consumed");
        expectRestingRemainder(book, 9, 98, 20, 8, context);
        expect(book.activeOrderCount() == 4, context + ": three asks and the remainder should be active");
        expectBookConsistent(book, context);
    }
    {
        // Limit beyond every execution price: the remainder rests at its own limit, not
        // at the price it last traded at.
        const std::string context = "buy sweeping both ask levels with a higher limit";
        OrderBook book = restingBook();
        const SubmissionResult result = book.submitOrder(Order(9, 103, 20, 1, Side::BUY));
        expectAccepted(result, 9, 3, 5, FillStatus::PARTIAL, context);
        expectTradePrices(result, {101, 101, 102}, context);
        expectRestingRemainder(book, 9, 103, 20, 5, context);
        expect(book.activeOrderCount() == 4, context + ": three bids and the remainder should be active");
        expectBookConsistent(book, context);
    }
    {
        const std::string context = "buy sweeping both ask levels exactly";
        OrderBook book = restingBook();
        const SubmissionResult result = book.submitOrder(Order(9, 102, 15, 1, Side::BUY));
        expectAccepted(result, 9, 3, 0, FillStatus::FILLED, context);
        expect(!book.hasAsks() && !isIndexed(book, 9), context + ": nothing should rest after an exact sweep");
        expect(book.activeOrderCount() == 3, context + ": only the three bids should be active");
        expectBookConsistent(book, context);
    }
}

// The active-order count follows resting orders through every kind of event: it rises
// only when an order rests and falls only when one fills completely or is cancelled.
void testActiveOrderCountThroughEvents() {
    const std::string context = "active-order count";
    OrderBook empty;
    expect(empty.activeOrderCount() == 0, context + ": an empty book should have no active orders");

    OrderBook book = restingBook();
    auto step = [&book, &context](std::size_t expected, const std::string& event) {
        expect(book.activeOrderCount() == expected, context + " after " + event + ": unexpected count");
        expect(book.snapshot(100).totalActiveVisibleOrders == expected,
               context + " after " + event + ": a full-depth snapshot should count the same orders");
        expectBookConsistent(book, context + " after " + event);
    };
    step(6, "setup");

    book.submitOrder(Order(10, 100, 3, 1, Side::BUY));
    step(7, "a passive submission rests");
    book.submitOrder(Order(11, 101, 6, 1, Side::BUY));
    step(6, "a taker fully fills one maker");
    book.submitOrder(Order(10, 101, 1, 1, Side::SELL));
    step(6, "a duplicate is rejected");
    Order malformed = crossingBuy();
    Access::setPrice(malformed, 0);
    book.submitOrder(malformed);
    step(6, "a malformed order is rejected");
    book.cancelOrder(2);
    step(5, "a cancellation");
    // Fills bids 10 (at 100) and 1 (at 99), then rests 1 at 99.
    book.submitOrder(Order(12, 99, 9, 1, Side::SELL));
    step(4, "a sweep fills two makers and rests a remainder");
    book.cancelOrder(12);
    step(3, "the remainder is cancelled");
    book.cancelOrder(12);
    step(3, "a repeat cancellation is refused");
    // Fills asks 5 (at 101) and 6 (at 102) exactly.
    book.submitOrder(Order(13, 102, 9, 1, Side::BUY));
    step(1, "a taker fully fills two makers across levels");
    book.addOrder(Order(14, 105, 1, 1, Side::SELL));
    step(2, "an add");
}

// addOrder refuses the same orders submitOrder rejects, but by throwing; its message
// should name the specific reason rather than lump the malformed cases together.
void testAddOrderNamesEachRejection() {
    const std::string context = "addOrder rejection message";
    OrderBook book = restingBook();

    std::vector<Order> refused;
    refused.push_back(crossingBuy());
    Access::setOrderId(refused.back(), 0);
    refused.push_back(crossingBuy());
    Access::setPrice(refused.back(), 0);
    refused.push_back(crossingBuy());
    Access::setQuantity(refused.back(), 0);
    refused.push_back(crossingBuy());
    Access::setSide(refused.back(), static_cast<Side>(2));
    refused.push_back(crossingBuy());
    Access::setOrderType(refused.back(), static_cast<OrderType>(1));
    refused.push_back(crossingBuy());
    refused.back().setFillStatus(FillStatus::FILLED);
    refused.push_back(crossingBuy(4));
    refused.push_back(crossingBuy());

    std::set<std::string> messages;
    for (const Order& order : refused) {
        const BookState before = capture(book);
        std::string message;
        try {
            book.addOrder(order);
        } catch (const std::invalid_argument& error) {
            message = error.what();
        }
        expect(!message.empty(), context + ": addOrder should throw std::invalid_argument with a message");
        expect(messages.insert(message).second, context + ": each rejection should have its own message, got a repeat: " + message);
        expect(capture(book) == before, context + ": a refused add should leave the book exactly as it was");
    }
}

// addOrder never matches, so it refuses an order at or through the best opposing price,
// which would leave the book crossed, and changes nothing. submitOrder matches the same order.
void testAddOrderRefusesToCross() {
    const std::string context = "addOrder crossing";
    OrderBook book = restingBook();
    const Order refused[] = {
        Order(9, 101, 1, 1, Side::BUY),   // at the best ask
        Order(9, 105, 1, 1, Side::BUY),   // through every ask
        Order(9, 99, 1, 1, Side::SELL),   // at the best bid
        Order(9, 90, 1, 1, Side::SELL),   // through every bid
    };
    for (const Order& order : refused) {
        const std::string orderContext = context + " (" + sideName(order.getSide()) + " at " +
                                         std::to_string(order.getPriceTick()) + ")";
        const BookState before = capture(book);
        std::string message;
        try {
            book.addOrder(order);
        } catch (const std::invalid_argument& error) {
            message = error.what();
        }
        expect(message == order_book::rejectionMessage(RejectionReason::WOULD_CROSS_BOOK),
               orderContext + ": addOrder should throw the WOULD_CROSS_BOOK message, got: " + message);
        expect(capture(book) == before, orderContext + ": a refused add should leave the book exactly as it was");
        expectBookConsistent(book, orderContext);
    }

    // Inside the spread it may still be added, and then it is the best bid.
    book.addOrder(Order(9, 100, 1, 1, Side::BUY));
    expect(book.getBestBid().getOrderId() == 9, context + ": an order inside the spread should be added");
    expectBookConsistent(book, context + " inside the spread");
}

}  // namespace

// An OrderRequest is a plain aggregate: building one never throws, and every check runs
// when it is submitted.
static_assert(std::is_aggregate_v<order_book::OrderRequest>, "OrderRequest should be a plain aggregate");
static_assert(std::is_nothrow_copy_constructible_v<order_book::OrderRequest>,
              "copying an OrderRequest should never throw");

// Submits a request the book must reject and checks that nothing observable changed.
void expectRequestRejectedUnchanged(OrderBook& book, const order_book::OrderRequest& request, RejectionReason reason,
                                    const std::string& context) {
    const BookState before = capture(book);
    const SubmissionResult result = book.submitOrder(request);
    expectRejected(result, request.id, request.quantity, reason, context);
    expect(result.getEvents() == std::vector<order_book::BookEvent>{order_book::OrderRejected{request.id, reason}},
           context + ": a rejected request should report exactly one OrderRejected");
    expect(capture(book) == before, context + ": a rejected request should leave the book exactly as it was");
    expectBookConsistent(book, context);
}

// The request's checks run in a fixed order, and the first that fails names the rejection.
void testMalformedRequestsAreRejectedInOrder() {
    using order_book::OrderRequest;
    const std::string context = "malformed request";
    OrderBook book = restingBook();
    const OrderType badType = static_cast<OrderType>(7);
    const Side badSide = static_cast<Side>(2);

    expectRequestRejectedUnchanged(book, OrderRequest{0, Side::BUY, 102, 20}, RejectionReason::INVALID_ORDER_ID,
                                   context + " reserved id 0");
    expectRequestRejectedUnchanged(book, OrderRequest{0, badSide, 0, 0, badType}, RejectionReason::INVALID_ORDER_ID,
                                   context + " id 0 before every other check");
    for (Side side : {Side::BUY, Side::SELL}) {
        expectRequestRejectedUnchanged(book, OrderRequest{9, side, 0, 5}, RejectionReason::INVALID_PRICE,
                                       context + " zero price " + sideName(side));
    }
    expectRequestRejectedUnchanged(book, OrderRequest{9, badSide, 0, 0, badType}, RejectionReason::INVALID_PRICE,
                                   context + " zero price, with an invalid type, before quantity");
    expectRequestRejectedUnchanged(book, OrderRequest{9, Side::BUY, 102, 0}, RejectionReason::INVALID_QUANTITY,
                                   context + " zero quantity");
    expectRequestRejectedUnchanged(book, OrderRequest{9, badSide, 102, 0, badType}, RejectionReason::INVALID_QUANTITY,
                                   context + " zero quantity before side and type");
    for (int rawSide : {2, -1, 255}) {
        expectRequestRejectedUnchanged(book, OrderRequest{9, static_cast<Side>(rawSide), 102, 20},
                                       RejectionReason::INVALID_SIDE, context + " side " + std::to_string(rawSide));
    }
    expectRequestRejectedUnchanged(book, OrderRequest{9, badSide, 102, 20, badType}, RejectionReason::INVALID_SIDE,
                                   context + " invalid side before type");
    expectRequestRejectedUnchanged(book, OrderRequest{9, Side::BUY, 102, 20, badType},
                                   RejectionReason::UNSUPPORTED_ORDER_TYPE, context + " type not an enumerator");
    const order_book::TimeInForce badTimeInForce = static_cast<order_book::TimeInForce>(9);
    using order_book::TimeInForce;
    // A market order needs no price, so its zero price passes check 2; it must carry none (6)
    // and must not be GTC (7b, after 7a's enumerator check).
    expectRequestRejectedUnchanged(book, OrderRequest{9, Side::BUY, 0, 0, OrderType::MARKET, TimeInForce::IOC},
                                   RejectionReason::INVALID_QUANTITY, context + " market with zero quantity");
    expectRequestRejectedUnchanged(book, OrderRequest{9, badSide, 0, 20, OrderType::MARKET, TimeInForce::IOC},
                                   RejectionReason::INVALID_SIDE, context + " market with invalid side");
    expectRequestRejectedUnchanged(book, OrderRequest{9, Side::BUY, 5, 20, OrderType::MARKET, TimeInForce::IOC},
                                   RejectionReason::PRICE_NOT_ALLOWED, context + " market with a price");
    expectRequestRejectedUnchanged(book, OrderRequest{9, Side::SELL, 5, 20, OrderType::MARKET, badTimeInForce},
                                   RejectionReason::PRICE_NOT_ALLOWED, context + " market price before time in force");
    expectRequestRejectedUnchanged(book, OrderRequest{9, Side::SELL, 0, 20, OrderType::MARKET, badTimeInForce},
                                   RejectionReason::INVALID_TIME_IN_FORCE, context + " market with invalid time in force");
    expectRequestRejectedUnchanged(book, OrderRequest{9, Side::BUY, 0, 20, OrderType::MARKET, TimeInForce::GTC},
                                   RejectionReason::TIME_IN_FORCE_NOT_ALLOWED, context + " market GTC");
    expectRequestRejectedUnchanged(book, OrderRequest{9, Side::BUY, 0, 20, OrderType::MARKET},
                                   RejectionReason::TIME_IN_FORCE_NOT_ALLOWED, context + " market with the default GTC");
    expectRequestRejectedUnchanged(book, OrderRequest{4, Side::BUY, 0, 20, OrderType::MARKET, TimeInForce::GTC},
                                   RejectionReason::TIME_IN_FORCE_NOT_ALLOWED, context + " market GTC before duplicate");
    expectRequestRejectedUnchanged(book, OrderRequest{4, Side::BUY, 0, 20, OrderType::MARKET, TimeInForce::IOC},
                                   RejectionReason::DUPLICATE_ORDER_ID, context + " market duplicate");

    // Post-only only on a GTC limit order (check 9), after the time-in-force checks.
    const auto postOnly = [](OrderRequest request) {
        request.postOnly = true;
        return request;
    };
    expectRequestRejectedUnchanged(book, postOnly(OrderRequest{9, Side::BUY, 102, 20, OrderType::LIMIT, TimeInForce::IOC}),
                                   RejectionReason::POST_ONLY_NOT_ALLOWED, context + " post-only IOC");
    expectRequestRejectedUnchanged(book, postOnly(OrderRequest{9, Side::BUY, 102, 20, OrderType::LIMIT, TimeInForce::FOK}),
                                   RejectionReason::POST_ONLY_NOT_ALLOWED, context + " post-only FOK");
    expectRequestRejectedUnchanged(book, postOnly(OrderRequest{9, Side::SELL, 0, 20, OrderType::MARKET, TimeInForce::IOC}),
                                   RejectionReason::POST_ONLY_NOT_ALLOWED, context + " post-only market");
    expectRequestRejectedUnchanged(book, postOnly(OrderRequest{9, Side::SELL, 0, 20, OrderType::MARKET, TimeInForce::GTC}),
                                   RejectionReason::TIME_IN_FORCE_NOT_ALLOWED, context + " market GTC before post-only");
    expectRequestRejectedUnchanged(book, postOnly(OrderRequest{9, Side::SELL, 5, 20, OrderType::LIMIT, badTimeInForce}),
                                   RejectionReason::INVALID_TIME_IN_FORCE, context + " time in force before post-only");
    expectRequestRejectedUnchanged(book, postOnly(OrderRequest{4, Side::BUY, 102, 20, OrderType::LIMIT, TimeInForce::IOC}),
                                   RejectionReason::POST_ONLY_NOT_ALLOWED, context + " post-only before duplicate");
    expectRequestRejectedUnchanged(book, postOnly(OrderRequest{4, Side::BUY, 50, 20}), RejectionReason::DUPLICATE_ORDER_ID,
                                   context + " post-only duplicate");

    // An invalid self-trade prevention mode is malformed whatever the owner (check 10, last).
    const auto withMode = [](OrderRequest request, order_book::OwnerId owner, int mode) {
        request.owner = owner;
        request.selfTradePrevention = static_cast<order_book::SelfTradePrevention>(mode);
        return request;
    };
    expectRequestRejectedUnchanged(book, withMode(OrderRequest{9, Side::BUY, 102, 20}, 0, 3),
                                   RejectionReason::INVALID_SELF_TRADE_PREVENTION, context + " invalid mode, no owner");
    expectRequestRejectedUnchanged(book, withMode(OrderRequest{9, Side::BUY, 102, 20}, 7, -1),
                                   RejectionReason::INVALID_SELF_TRADE_PREVENTION, context + " invalid mode, an owner");
    expectRequestRejectedUnchanged(book,
                                   withMode(postOnly(OrderRequest{9, Side::BUY, 102, 20, OrderType::LIMIT, TimeInForce::IOC}),
                                            7, 3),
                                   RejectionReason::POST_ONLY_NOT_ALLOWED, context + " post-only before the mode");
    expectRequestRejectedUnchanged(book, withMode(OrderRequest{4, Side::BUY, 102, 20}, 7, 3),
                                   RejectionReason::INVALID_SELF_TRADE_PREVENTION, context + " mode before duplicate");
    expectRequestRejectedUnchanged(book, OrderRequest{9, Side::BUY, 102, 20, badType, badTimeInForce},
                                   RejectionReason::UNSUPPORTED_ORDER_TYPE, context + " type before time in force");
    expectRequestRejectedUnchanged(book, OrderRequest{9, Side::BUY, 102, 20, OrderType::LIMIT, badTimeInForce},
                                   RejectionReason::INVALID_TIME_IN_FORCE, context + " time in force not an enumerator");
    expectRequestRejectedUnchanged(book, OrderRequest{4, Side::BUY, 102, 20, OrderType::LIMIT, badTimeInForce},
                                   RejectionReason::INVALID_TIME_IN_FORCE, context + " time in force before duplicate");

    // Malformed is reported ahead of duplicate when a request is both; then a live ID.
    expectRequestRejectedUnchanged(book, OrderRequest{4, Side::BUY, 0, 20}, RejectionReason::INVALID_PRICE,
                                   context + " and duplicate");
    expectRequestRejectedUnchanged(book, OrderRequest{4, Side::BUY, 102, 20}, RejectionReason::DUPLICATE_ORDER_ID,
                                   "duplicate request");
    expectRequestRejectedUnchanged(book, OrderRequest{1, Side::SELL, 50, 1}, RejectionReason::DUPLICATE_ORDER_ID,
                                   "duplicate request on the other side");
}

// What a book reports, without the addresses of its orders, so two books can be compared.
BookState withoutAddresses(BookState state) {
    for (auto* side : {&state.bids, &state.asks}) {
        for (LevelState& level : *side) {
            for (OrderState& order : level.orders) {
                order.address = nullptr;
            }
        }
    }
    for (auto& entry : state.index) {
        entry.second = nullptr;
    }
    return state;
}

void expectSameResult(const SubmissionResult& viaOrder, const SubmissionResult& viaRequest, const std::string& context) {
    expect(viaRequest.getStatus() == viaOrder.getStatus() && viaRequest.getRejectionReason() == viaOrder.getRejectionReason()
               && viaRequest.getSubmissionId() == viaOrder.getSubmissionId()
               && viaRequest.getRemainingQuantity() == viaOrder.getRemainingQuantity()
               && viaRequest.getFillStatus() == viaOrder.getFillStatus(),
           context + ": the two overloads should give the same outcome");
    expect(viaRequest.getEvents() == viaOrder.getEvents(), context + ": the two overloads should give the same events");
    const auto& orderTrades = viaOrder.getGeneratedTrades();
    const auto& requestTrades = viaRequest.getGeneratedTrades();
    expect(orderTrades.size() == requestTrades.size(), context + ": the two overloads should make as many trades");
    for (std::size_t i = 0; i < orderTrades.size(); ++i) {
        const order_book::Trade& left = orderTrades[i];
        const order_book::Trade& right = requestTrades[i];
        expect(left.getTradeId() == right.getTradeId() && left.getRestingId() == right.getRestingId()
                   && left.getIncomingId() == right.getIncomingId()
                   && left.getExecutionPrice() == right.getExecutionPrice()
                   && left.getExecutionQuantity() == right.getExecutionQuantity()
                   && left.getIncomingSide() == right.getIncomingSide(),
               context + ": the two overloads should make the same trades");
    }
}

// submitOrder(Order) is the request with the same ID, side, price, and quantity and every
// default: two books fed the same script, one through each overload, stay identical.
void testRequestOverloadMatchesOrderOverload() {
    struct Step {
        OrderId id;
        Side side;
        Price price;
        unsigned int quantity;
    };
    const std::vector<Step> script = {
        {10, Side::BUY, 97, 4},    // rests below the bids
        {11, Side::SELL, 101, 3},  // rests behind the best asks
        {12, Side::BUY, 101, 9},   // sweeps 101 and rests the rest
        {13, Side::SELL, 98, 30},  // sweeps every bid level and rests
        {13, Side::BUY, 50, 1},    // duplicate of a resting ID
        {14, Side::BUY, 0, 5},     // malformed
        {12, Side::SELL, 120, 2},  // filled earlier, so free again
        {15, Side::BUY, 130, 50},  // sweeps the asks
    };
    OrderBook viaOrder = restingBook();
    OrderBook viaRequest = restingBook();
    for (std::size_t i = 0; i < script.size(); ++i) {
        const Step& step = script[i];
        const std::string context = "overload step " + std::to_string(i);
        Order incoming(1, 1, 1, 1, Side::BUY);
        Access::setOrderId(incoming, step.id);
        Access::setPrice(incoming, step.price);
        Access::setQuantity(incoming, step.quantity);
        Access::setSide(incoming, step.side);
        const SubmissionResult orderResult = viaOrder.submitOrder(incoming);
        const SubmissionResult requestResult =
            viaRequest.submitOrder(order_book::OrderRequest{step.id, step.side, step.price, step.quantity});
        expectSameResult(orderResult, requestResult, context);
        expect(withoutAddresses(capture(viaRequest)) == withoutAddresses(capture(viaOrder)),
               context + ": the two overloads should leave identical books");
        expectBookConsistent(viaRequest, context);
    }
}

int main() {
    testMalformedRequestsAreRejectedInOrder();
    testRequestOverloadMatchesOrderOverload();
    testDuplicateActiveIdIsRejected();
    testReusedCancelledIdIsAccepted();
    testReusedFilledIdIsAccepted();
    testMalformedOrdersAreRejected();
    testPartiallyFilledIncomingRestsIndexed();
    testMultipleLevelSweep();
    testActiveOrderCountThroughEvents();
    testAddOrderNamesEachRejection();
    testAddOrderRefusesToCross();
    for (Side side : {Side::BUY, Side::SELL}) {
        testFullyFilledIncomingIsNotIndexed(side);
    }

    std::cout << "submission tests passed" << std::endl;
    return EXIT_SUCCESS;
}
