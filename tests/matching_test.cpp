#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <list>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "order_book/order.hpp"
#include "order_book/order_book.hpp"

namespace order_book {

// Befriended by OrderBook and PriceLevel so these tests can inspect internals.
struct OrderBookTestAccess {
    static const auto& index(const OrderBook& book) { return book.orderIndex; }
    static const auto& bids(const OrderBook& book) { return book.bids; }
    static const auto& asks(const OrderBook& book) { return book.asks; }
    static TradeId nextTradeId(const OrderBook& book) { return book.nextTradeId; }
    static void setNextTradeId(OrderBook& book, TradeId next) { book.nextTradeId = next; }
    static SequenceNum nextSequenceNum(const OrderBook& book) { return book.nextSequenceNum; }
    static void setNextSequenceNum(OrderBook& book, SequenceNum next) { book.nextSequenceNum = next; }
    // The resting order itself (findActiveOrder only returns a copy), to check identity.
    static const Order& indexedOrder(const OrderBook& book, OrderId orderId) { return *book.orderIndex.at(orderId).itr; }

    static const std::list<Order>& orders(const PriceLevel& level) { return level.orders; }
};

}  // namespace order_book

namespace {

using Access = order_book::OrderBookTestAccess;
using order_book::FillStatus;
using order_book::Order;
using order_book::OrderBook;
using order_book::PriceLevel;
using order_book::Side;
using order_book::SubmissionStatus;
using order_book::Trade;
using order_book::TradeId;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

template <typename Callable>
void expectThrows(Callable&& callable, const std::string& message) {
    try {
        callable();
    } catch (const std::exception&) {
        return;
    }

    std::cerr << "FAILED: " << message << std::endl;
    std::exit(EXIT_FAILURE);
}

// Same invariants as execution_test (one index entry per resting order, pointing at it,
// no leftover entries, no empty levels, aggregates matching their orders), plus time
// priority: within a level, sequence numbers strictly increase from front to back, and
// no two resting orders share one.
template <typename Levels>
void expectSideConsistent(const OrderBook& book, const Levels& levels, Side side, std::size_t& liveOrders,
                          std::set<unsigned int>& sequenceNums, const std::string& context) {
    for (const auto& [price, level] : levels) {
        expect(!level.isEmpty(), context + ": no empty price level should remain in the book");

        unsigned int remainingSum = 0;
        unsigned int previousSequence = 0;
        for (const Order& order : Access::orders(level)) {
            ++liveOrders;
            remainingSum += order.getRemainingQuantity();
            expect(order.getRemainingQuantity() > 0, context + ": no fully filled order should rest in a level");
            expect(order.getSequenceNum() > previousSequence,
                   context + ": a level's orders should be in acceptance (sequence) order");
            previousSequence = order.getSequenceNum();
            expect(sequenceNums.insert(order.getSequenceNum()).second,
                   context + ": no two resting orders should share a sequence number");

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

// The book is internally consistent and never left crossed: matching runs until the
// incoming order stops crossing, so a resting remainder must sit inside the spread.
void expectBookConsistent(const OrderBook& book, const std::string& context) {
    std::size_t liveOrders = 0;
    std::set<unsigned int> sequenceNums;
    expectSideConsistent(book, Access::bids(book), Side::BUY, liveOrders, sequenceNums, context);
    expectSideConsistent(book, Access::asks(book), Side::SELL, liveOrders, sequenceNums, context);
    expect(Access::index(book).size() == liveOrders,
           context + ": the index should hold exactly one entry per live resting order");
    if (book.hasBids() && book.hasAsks()) {
        expect(book.getBestBid().getPriceTick() < book.getBestAsk().getPriceTick(),
               context + ": the book should never be left crossed");
    }
    // The library's checker should agree with these hand-written checks.
    const order_book::InvariantResult invariants = book.checkInvariants();
    expect(invariants.isValid(),
           context + ": the book should pass its invariant check, got " + order_book::formatInvariantResult(invariants));
}

bool isIndexed(const OrderBook& book, unsigned int orderId) {
    return Access::index(book).find(orderId) != Access::index(book).end();
}

Side opposite(Side side) {
    return side == Side::BUY ? Side::SELL : Side::BUY;
}

std::string sideName(Side side) {
    return side == Side::BUY ? "buy" : "sell";
}

bool hasSide(const OrderBook& book, Side side) {
    return side == Side::BUY ? book.hasBids() : book.hasAsks();
}

Order bestOn(const OrderBook& book, Side side) {
    return side == Side::BUY ? book.getBestBid() : book.getBestAsk();
}

const PriceLevel& levelAt(const OrderBook& book, Side side, unsigned int price) {
    return side == Side::BUY ? Access::bids(book).at(price) : Access::asks(book).at(price);
}

// Opposing liquidity is set up at 100 and `ticksAway` steps further from the incoming
// side (asks rise, bids fall), so an incoming limit at priceAway(side, n) crosses every
// opposing level up to n, and priceAway(side, -1) crosses none.
unsigned int priceAway(Side incomingSide, int ticksAway) {
    return static_cast<unsigned int>(incomingSide == Side::BUY ? 100 + ticksAway : 100 - ticksAway);
}

struct ExpectedTrade {
    unsigned int restingId;
    unsigned int price;
    unsigned int quantity;
};

// Checks the trades one submission returned, in order: each against the expected maker,
// price, and quantity, all naming `incoming` as the taker, with consecutive trade IDs
// from `firstTradeId`, never through the incoming limit, and never improving on an
// earlier execution's price (best opposing price first).
void expectTrades(const std::vector<Trade>& trades, const Order& incoming, TradeId firstTradeId,
                  std::initializer_list<ExpectedTrade> expected, const std::string& context) {
    expect(trades.size() == expected.size(), context + ": should produce " + std::to_string(expected.size()) +
                                                 " trade(s), got " + std::to_string(trades.size()));
    const bool incomingBuys = incoming.getSide() == Side::BUY;
    std::size_t i = 0;
    for (const ExpectedTrade& want : expected) {
        const Trade& trade = trades[i];
        const std::string at = context + ": trade " + std::to_string(i);
        expect(trade.getTradeId() == firstTradeId + i, at + " should have the next consecutive trade ID");
        expect(trade.getRestingId() == want.restingId, at + " should execute against resting order " +
                                                           std::to_string(want.restingId));
        expect(trade.getExecutionPrice() == want.price, at + " should execute at " + std::to_string(want.price));
        expect(trade.getExecutionQuantity() == want.quantity,
               at + " should execute quantity " + std::to_string(want.quantity));
        expect(trade.getIncomingId() == incoming.getOrderId() && trade.getIncomingSide() == incoming.getSide(),
               at + " should name the submitted order as the taker");
        expect(incomingBuys ? trade.getExecutionPrice() <= incoming.getPriceTick()
                            : trade.getExecutionPrice() >= incoming.getPriceTick(),
               at + " should never execute through the incoming limit price");
        if (i > 0) {
            const unsigned int previous = trades[i - 1].getExecutionPrice();
            expect(incomingBuys ? trade.getExecutionPrice() >= previous : trade.getExecutionPrice() <= previous,
                   at + " should not execute at a better price than an earlier trade");
        }
        ++i;
    }
}

void testExactMatch(Side incomingSide) {
    const std::string context = "exact match (incoming " + sideName(incomingSide) + ")";
    OrderBook book{};
    book.addOrder(Order(1, 100, 10, 1, opposite(incomingSide)));
    const Order incoming(2, 100, 10, 1, incomingSide);

    const std::vector<Trade> trades = book.submitOrder(incoming).getGeneratedTrades();

    expectTrades(trades, incoming, 1, {{1, 100, 10}}, context);
    expect(!book.hasBids() && !book.hasAsks(), context + ": both orders should be gone, leaving the book empty");
    expect(!isIndexed(book, 1) && !isIndexed(book, 2), context + ": neither order should remain indexed");
    expectBookConsistent(book, context);
}

void testIncomingPartialFill(Side incomingSide) {
    const std::string context = "incoming partial fill (incoming " + sideName(incomingSide) + ")";
    OrderBook book{};
    book.addOrder(Order(1, 100, 4, 1, opposite(incomingSide)));
    const Order incoming(2, priceAway(incomingSide, 2), 10, 1, incomingSide);

    const std::vector<Trade> trades = book.submitOrder(incoming).getGeneratedTrades();

    expectTrades(trades, incoming, 1, {{1, 100, 4}}, context);
    expect(!hasSide(book, opposite(incomingSide)), context + ": the filled resting order's level should be erased");

    const Order remainder = book.findActiveOrder(2);
    expect(remainder.getPriceTick() == priceAway(incomingSide, 2),
           context + ": the remainder should rest at its own limit price, not the execution price");
    expect(remainder.getSide() == incomingSide, context + ": the remainder should rest on the incoming side");
    expect(remainder.getRemainingQuantity() == 6 && remainder.getInitialQuantity() == 10,
           context + ": the remainder should keep 6 of its original 10");
    expect(remainder.getExecutionLevel() == FillStatus::PARTIAL, context + ": the remainder should be PARTIAL");
    expect(bestOn(book, incomingSide).getOrderId() == 2, context + ": the remainder should be the new best order");
    expectBookConsistent(book, context);
}

void testRestingPartialFill(Side incomingSide) {
    const std::string context = "resting partial fill (incoming " + sideName(incomingSide) + ")";
    const Side restingSide = opposite(incomingSide);
    OrderBook book{};
    book.addOrder(Order(1, 100, 10, 1, restingSide));
    book.addOrder(Order(3, 100, 5, 1, restingSide));
    const Order* restingBefore = &Access::indexedOrder(book, 1);
    const Order incoming(2, 100, 4, 1, incomingSide);

    const std::vector<Trade> trades = book.submitOrder(incoming).getGeneratedTrades();

    expectTrades(trades, incoming, 1, {{1, 100, 4}}, context);
    expect(!isIndexed(book, 2) && !hasSide(book, incomingSide), context + ": a fully filled incoming order should not rest");

    const Order& resting = Access::indexedOrder(book, 1);
    expect(&resting == restingBefore, context + ": the resting order should be filled in place");
    expect(resting.getRemainingQuantity() == 6 && resting.getExecutionLevel() == FillStatus::PARTIAL,
           context + ": the resting order should be left PARTIAL with 6");
    const PriceLevel& level = levelAt(book, restingSide, 100);
    expect(level.getOldestOrder().getOrderId() == 1, context + ": the resting order should keep its FIFO position");
    expect(level.getAggregateQuantity() == 11 && level.getOrdersQuantity() == 2,
           context + ": the level should drop from 15 to 11 and keep both orders");
    expectBookConsistent(book, context);
}

void testMultipleMatchesAtOnePrice(Side incomingSide) {
    const std::string context = "one price, many orders (incoming " + sideName(incomingSide) + ")";
    const Side restingSide = opposite(incomingSide);
    OrderBook book{};
    // IDs out of order: time priority follows acceptance, not ID.
    book.addOrder(Order(9, 100, 3, 1, restingSide));
    book.addOrder(Order(4, 100, 4, 1, restingSide));
    book.addOrder(Order(7, 100, 5, 1, restingSide));
    const Order incoming(20, 100, 10, 1, incomingSide);

    const std::vector<Trade> trades = book.submitOrder(incoming).getGeneratedTrades();

    expectTrades(trades, incoming, 1, {{9, 100, 3}, {4, 100, 4}, {7, 100, 3}}, context);
    expect(!isIndexed(book, 9) && !isIndexed(book, 4), context + ": the fully filled orders should be unindexed");
    const Order last = book.findActiveOrder(7);
    expect(last.getRemainingQuantity() == 2 && last.getExecutionLevel() == FillStatus::PARTIAL,
           context + ": the last order reached should be left PARTIAL with 2");
    expect(levelAt(book, restingSide, 100).getAggregateQuantity() == 2, context + ": the level should hold 2");
    expect(!hasSide(book, incomingSide), context + ": a fully filled incoming order should not rest");
    expectBookConsistent(book, context);
}

void testMultiplePriceLevels(Side incomingSide) {
    const std::string context = "many price levels (incoming " + sideName(incomingSide) + ")";
    const Side restingSide = opposite(incomingSide);
    OrderBook book{};
    // Added out of price order: price priority follows the level, not acceptance.
    book.addOrder(Order(9, priceAway(incomingSide, 3), 5, 1, restingSide));
    book.addOrder(Order(5, priceAway(incomingSide, 1), 3, 1, restingSide));
    book.addOrder(Order(1, priceAway(incomingSide, 0), 2, 1, restingSide));
    book.addOrder(Order(7, priceAway(incomingSide, 2), 4, 1, restingSide));
    book.addOrder(Order(3, priceAway(incomingSide, 0), 1, 1, restingSide));
    const Order incoming(2, priceAway(incomingSide, 2), 20, 1, incomingSide);

    const std::vector<Trade> trades = book.submitOrder(incoming).getGeneratedTrades();

    expectTrades(trades, incoming, 1,
                 {{1, priceAway(incomingSide, 0), 2},
                  {3, priceAway(incomingSide, 0), 1},
                  {5, priceAway(incomingSide, 1), 3},
                  {7, priceAway(incomingSide, 2), 4}},
                 context);

    // Stops at the first level beyond the limit, though quantity remains.
    const Order untouched = bestOn(book, restingSide);
    expect(untouched.getOrderId() == 9 && untouched.getRemainingQuantity() == 5,
           context + ": the level beyond the incoming limit should be untouched");

    const Order remainder = book.findActiveOrder(2);
    expect(remainder.getPriceTick() == priceAway(incomingSide, 2) && remainder.getRemainingQuantity() == 10,
           context + ": the remaining 10 should rest at the incoming limit price");
    expectBookConsistent(book, context);
}

void testPriceBeatsTimePriority(Side incomingSide) {
    const std::string context = "price before time (incoming " + sideName(incomingSide) + ")";
    const Side restingSide = opposite(incomingSide);
    OrderBook book{};
    book.addOrder(Order(1, priceAway(incomingSide, 1), 5, 1, restingSide));  // older, worse price
    book.addOrder(Order(3, priceAway(incomingSide, 0), 5, 1, restingSide));  // newer, better price
    const Order incoming(2, priceAway(incomingSide, 1), 5, 1, incomingSide);

    const std::vector<Trade> trades = book.submitOrder(incoming).getGeneratedTrades();

    expectTrades(trades, incoming, 1, {{3, priceAway(incomingSide, 0), 5}}, context);
    expect(isIndexed(book, 1) && book.findActiveOrder(1).getRemainingQuantity() == 5,
           context + ": the older order at a worse price should be untouched");
    expectBookConsistent(book, context);
}

void testNonCrossingOrderRests(Side incomingSide) {
    const std::string context = "non-crossing (incoming " + sideName(incomingSide) + ")";
    const Side restingSide = opposite(incomingSide);
    OrderBook book{};
    book.addOrder(Order(1, 100, 5, 1, restingSide));
    const Order incoming(2, priceAway(incomingSide, -1), 7, 1, incomingSide);

    const std::vector<Trade> trades = book.submitOrder(incoming).getGeneratedTrades();

    expect(trades.empty(), context + ": an order one tick short of the best opposing price should not trade");
    expect(Access::nextTradeId(book) == 1, context + ": no trade ID should be consumed");
    const Order rested = book.findActiveOrder(2);
    expect(rested.getPriceTick() == priceAway(incomingSide, -1) && rested.getRemainingQuantity() == 7 &&
               rested.getExecutionLevel() == FillStatus::NONE,
           context + ": the whole order should rest unfilled at its limit price");
    expect(book.findActiveOrder(1).getRemainingQuantity() == 5, context + ": the opposing order should be untouched");
    expectBookConsistent(book, context);

    OrderBook empty{};
    const Order lonely(1, 100, 3, 1, incomingSide);
    expect(empty.submitOrder(lonely).getGeneratedTrades().empty(), context + ": an order into an empty opposing side should not trade");
    expect(bestOn(empty, incomingSide).getRemainingQuantity() == 3, context + ": it should rest in full");
    expectBookConsistent(empty, context + " (empty book)");
}

void testSweepingWholeSideRestsRemainder(Side incomingSide) {
    const std::string context = "sweep whole side (incoming " + sideName(incomingSide) + ")";
    const Side restingSide = opposite(incomingSide);
    OrderBook book{};
    book.addOrder(Order(1, priceAway(incomingSide, 0), 3, 1, restingSide));
    book.addOrder(Order(3, priceAway(incomingSide, 1), 4, 1, restingSide));
    const Order incoming(2, priceAway(incomingSide, 5), 10, 1, incomingSide);

    const std::vector<Trade> trades = book.submitOrder(incoming).getGeneratedTrades();

    expectTrades(trades, incoming, 1, {{1, priceAway(incomingSide, 0), 3}, {3, priceAway(incomingSide, 1), 4}},
                 context);
    expect(!hasSide(book, restingSide), context + ": matching should stop once the opposing side is empty");
    const Order remainder = book.findActiveOrder(2);
    expect(remainder.getPriceTick() == priceAway(incomingSide, 5) && remainder.getRemainingQuantity() == 3,
           context + ": the remaining 3 should rest at the limit price, beyond every execution price");
    expectBookConsistent(book, context);
}

void testRemainderLaterTradesAsMaker(Side incomingSide) {
    const std::string context = "remainder as maker (incoming " + sideName(incomingSide) + ")";
    const Side restingSide = opposite(incomingSide);
    OrderBook book{};
    book.addOrder(Order(1, 100, 3, 1, restingSide));
    book.submitOrder(Order(2, priceAway(incomingSide, 1), 8, 1, incomingSide));  // 5 rests

    const Order taker(5, priceAway(incomingSide, 1), 5, 1, restingSide);
    const std::vector<Trade> trades = book.submitOrder(taker).getGeneratedTrades();

    expectTrades(trades, taker, 2, {{2, priceAway(incomingSide, 1), 5}}, context);
    expect(!book.hasBids() && !book.hasAsks(), context + ": both orders should be fully filled");
    expectBookConsistent(book, context);
}

void testSequenceNumbers() {
    const std::string context = "sequence numbers";
    OrderBook book{};

    // The book numbers orders in acceptance order, replacing the caller's value.
    book.addOrder(Order(1, 100, 5, 77, Side::SELL));
    book.addOrder(Order(2, 101, 5, 77, Side::SELL));
    expect(book.findActiveOrder(1).getSequenceNum() == 1, context + ": the first accepted order should be numbered 1");
    expect(book.findActiveOrder(2).getSequenceNum() == 2, context + ": the next accepted order should be numbered 2");

    book.submitOrder(Order(3, 90, 5, 77, Side::BUY));
    expect(book.findActiveOrder(3).getSequenceNum() == 3, context + ": a submitted order that rests should be numbered 3");

    // A submission that fills completely was still accepted, so it uses up a number.
    book.submitOrder(Order(4, 100, 5, 1, Side::BUY));
    expect(Access::nextSequenceNum(book) == 5, context + ": a fully filled submission should consume a number");

    // Rejected orders were never accepted, so they consume nothing.
    expect(book.submitOrder(Order(3, 95, 5, 1, Side::BUY)).getStatus() == SubmissionStatus::REJECTED,
           context + ": submitting a duplicate resting ID should be rejected");
    expectThrows([&book] { book.addOrder(Order(2, 102, 5, 1, Side::SELL)); },
                 context + ": adding a duplicate resting ID should throw");
    Order inactive(8, 100, 5, 1, Side::BUY);
    inactive.setExecutionLevel(FillStatus::FILLED);
    expect(book.submitOrder(inactive).getStatus() == SubmissionStatus::REJECTED,
           context + ": submitting an inactive order should be rejected");
    expect(Access::nextSequenceNum(book) == 5, context + ": rejected orders should not consume a number");

    // A remainder keeps the number it was given before matching.
    book.submitOrder(Order(6, 101, 8, 1, Side::BUY));
    const Order remainder = book.findActiveOrder(6);
    expect(remainder.getRemainingQuantity() == 3 && remainder.getSequenceNum() == 5,
           context + ": a partially filled remainder should rest with the number assigned on acceptance");

    // A resting order keeps its number when partially filled.
    book.addOrder(Order(7, 105, 10, 1, Side::SELL));
    book.submitOrder(Order(8, 105, 4, 1, Side::BUY));
    const Order partiallyFilled = book.findActiveOrder(7);
    expect(partiallyFilled.getRemainingQuantity() == 6 && partiallyFilled.getSequenceNum() == 6,
           context + ": a partially filled resting order should keep its number");

    // Later arrivals at the same price queue behind it, in number order.
    book.submitOrder(Order(9, 101, 2, 1, Side::BUY));
    expect(book.findActiveOrder(9).getSequenceNum() == 8, context + ": numbering should continue without gaps");
    const std::list<Order>& queue = Access::orders(levelAt(book, Side::BUY, 101));
    expect(queue.size() == 2 && queue.front().getOrderId() == 6 && queue.back().getOrderId() == 9,
           context + ": a later order at the same price should queue behind the remainder");
    expectBookConsistent(book, context);

    // An ID freed by a fill can be reused, and the new order gets a fresh number.
    book.submitOrder(Order(1, 100, 1, 1, Side::BUY));
    expect(book.findActiveOrder(1).getSequenceNum() == 9, context + ": a reused ID should get a fresh number");
    expectBookConsistent(book, context + " (reused ID)");
}

void testSequenceNumberWraps() {
    const std::string context = "sequence wrap";
    constexpr unsigned int maxSequence = std::numeric_limits<unsigned int>::max();
    OrderBook book{};
    Access::setNextSequenceNum(book, maxSequence);

    book.addOrder(Order(1, 100, 5, 1, Side::SELL));
    expect(book.findActiveOrder(1).getSequenceNum() == maxSequence, context + ": the last number should be used");
    expect(Access::nextSequenceNum(book) == 1, context + ": the counter should wrap past zero back to 1");

    book.submitOrder(Order(2, 100, 2, 1, Side::SELL));
    expect(book.findActiveOrder(2).getSequenceNum() == 1, context + ": the next order should silently reuse 1");
    expect(Access::nextSequenceNum(book) == 2, context + ": numbering should carry on from 1");

    // Time priority comes from queue position, so a wrapped (lower) number still queues
    // behind the order accepted before it.
    const std::list<Order>& queue = Access::orders(levelAt(book, Side::SELL, 100));
    expect(queue.size() == 2 && queue.front().getOrderId() == 1 && queue.back().getOrderId() == 2,
           context + ": the order accepted after the wrap should queue behind");
    const Order incoming(3, 100, 6, 1, Side::BUY);
    expectTrades(book.submitOrder(incoming).getGeneratedTrades(), incoming, 1, {{1, 100, 5}, {2, 100, 1}}, context);
}

void testTradeIdWraps() {
    const std::string context = "trade ID wrap";
    constexpr unsigned int maxTradeId = std::numeric_limits<unsigned int>::max();
    OrderBook book{};
    book.addOrder(Order(1, 100, 2, 1, Side::SELL));
    book.addOrder(Order(2, 101, 3, 1, Side::SELL));
    Access::setNextTradeId(book, maxTradeId);

    // Matching keeps working across the wrap rather than rejecting every crossing order.
    const std::vector<Trade> trades = book.submitOrder(Order(3, 101, 5, 1, Side::BUY)).getGeneratedTrades();
    expect(trades.size() == 2, context + ": both resting orders should be filled");
    expect(trades[0].getTradeId() == maxTradeId && trades[0].getRestingId() == 1,
           context + ": the first execution should use the last trade ID");
    expect(trades[1].getTradeId() == 1 && trades[1].getRestingId() == 2,
           context + ": the next execution should wrap past zero back to 1");
    expect(Access::nextTradeId(book) == 2, context + ": numbering should carry on from 1");
    expect(!book.hasBids() && !book.hasAsks(), context + ": the book should be empty");
    expectBookConsistent(book, context);
}

struct Submission {
    unsigned int id;
    Side side;
    unsigned int price;
    unsigned int quantity;
};

// Submissions mixing both sides, multi-level sweeps, and remainders that rest and are
// later hit, with the trades each one should produce.
const std::vector<Submission> kScript = {
    {1, Side::SELL, 102, 5},  // rests
    {2, Side::SELL, 101, 3},  // rests
    {3, Side::SELL, 101, 4},  // rests behind 2
    {4, Side::BUY, 99, 6},    // rests
    {5, Side::BUY, 101, 5},   // fills 2, then part of 3
    {6, Side::SELL, 98, 10},  // fills 4 at 99, rests 4 at 98
    {7, Side::BUY, 102, 9},   // fills 6 at 98, rest of 3 at 101, part of 1 at 102
    {8, Side::BUY, 100, 4},   // best ask is 102: rests
    {9, Side::SELL, 100, 1},  // fills part of 8
};

std::vector<Trade> runScript(OrderBook& book, const std::string& context) {
    std::vector<Trade> all;
    for (const Submission& step : kScript) {
        const std::vector<Trade> trades =
            book.submitOrder(Order(step.id, step.price, step.quantity, 1, step.side)).getGeneratedTrades();
        all.insert(all.end(), trades.begin(), trades.end());
        expectBookConsistent(book, context + " after order " + std::to_string(step.id));
    }
    return all;
}

void testDeterministicEventOrdering() {
    const std::string context = "event ordering";
    OrderBook book{};
    const std::vector<Trade> trades = runScript(book, context);

    const std::vector<ExpectedTrade> expected = {
        {2, 101, 3}, {3, 101, 2},                // order 5
        {4, 99, 6},                              // order 6
        {6, 98, 4},  {3, 101, 2}, {1, 102, 3},   // order 7
        {8, 100, 1},                             // order 9
    };
    const std::vector<unsigned int> takers = {5, 5, 6, 7, 7, 7, 9};
    expect(trades.size() == expected.size(), context + ": the script should produce 7 trades");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const std::string at = context + ": trade " + std::to_string(i);
        expect(trades[i].getTradeId() == i + 1, at + " should be numbered in execution order");
        expect(trades[i].getRestingId() == expected[i].restingId && trades[i].getIncomingId() == takers[i] &&
                   trades[i].getExecutionPrice() == expected[i].price &&
                   trades[i].getExecutionQuantity() == expected[i].quantity,
               at + " should match the expected maker, taker, price, and quantity");
    }

    const Order bid = book.getBestBid();
    const Order ask = book.getBestAsk();
    expect(bid.getOrderId() == 8 && bid.getRemainingQuantity() == 3 && bid.getSequenceNum() == 8,
           context + ": order 8 should rest with 3 left and its original number");
    expect(ask.getOrderId() == 1 && ask.getRemainingQuantity() == 2 && ask.getSequenceNum() == 1,
           context + ": order 1 should rest with 2 left and its original number");
    expect(Access::index(book).size() == 2, context + ": only orders 8 and 1 should remain");

    // Replaying the same input on a fresh book gives the same events and the same book.
    OrderBook replay{};
    const std::vector<Trade> replayed = runScript(replay, context + " (replay)");
    expect(replayed.size() == trades.size(), context + ": a replay should produce the same number of trades");
    for (std::size_t i = 0; i < trades.size(); ++i) {
        expect(replayed[i].getTradeId() == trades[i].getTradeId() &&
                   replayed[i].getRestingId() == trades[i].getRestingId() &&
                   replayed[i].getIncomingId() == trades[i].getIncomingId() &&
                   replayed[i].getExecutionPrice() == trades[i].getExecutionPrice() &&
                   replayed[i].getExecutionQuantity() == trades[i].getExecutionQuantity() &&
                   replayed[i].getIncomingSide() == trades[i].getIncomingSide(),
               context + ": replayed trade " + std::to_string(i) + " should be identical");
    }
    for (const unsigned int id : {1u, 8u}) {
        const Order original = book.findActiveOrder(id);
        const Order copy = replay.findActiveOrder(id);
        expect(copy.getSequenceNum() == original.getSequenceNum() &&
                   copy.getRemainingQuantity() == original.getRemainingQuantity() &&
                   copy.getPriceTick() == original.getPriceTick(),
               context + ": replayed order " + std::to_string(id) + " should rest identically");
    }
}

}  // namespace

int main() {
    for (const Side incomingSide : {Side::BUY, Side::SELL}) {
        testExactMatch(incomingSide);
        testIncomingPartialFill(incomingSide);
        testRestingPartialFill(incomingSide);
        testMultipleMatchesAtOnePrice(incomingSide);
        testMultiplePriceLevels(incomingSide);
        testPriceBeatsTimePriority(incomingSide);
        testNonCrossingOrderRests(incomingSide);
        testSweepingWholeSideRestsRemainder(incomingSide);
        testRemainderLaterTradesAsMaker(incomingSide);
    }
    testSequenceNumbers();
    testSequenceNumberWraps();
    testTradeIdWraps();
    testDeterministicEventOrdering();

    std::cout << "matching tests passed" << std::endl;
    return EXIT_SUCCESS;
}
