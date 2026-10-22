#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <list>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "order_book/cancellation_results.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"

namespace order_book {

// Befriended by OrderBook and PriceLevel so these tests can inspect internals.
struct OrderBookTestAccess {
    static const auto& index(const OrderBook& book) { return book.orderIndex; }
    static const auto& bids(const OrderBook& book) { return book.bids; }
    static const auto& asks(const OrderBook& book) { return book.asks; }
    static TradeId nextTradeId(const OrderBook& book) { return book.nextTradeId; }
    static SequenceNum nextSequenceNum(const OrderBook& book) { return book.nextSequenceNum; }
    // The resting order itself (findActiveOrder only returns a copy), to check identity.
    static const Order& indexedOrder(const OrderBook& book, OrderId orderId) { return *book.orderIndex.at(orderId).itr; }

    static const std::list<Order>& orders(const PriceLevel& level) { return level.orders; }
};

}  // namespace order_book

namespace {

using Access = order_book::OrderBookTestAccess;
using order_book::CancellationResult;
using order_book::FillStatus;
using order_book::Order;
using order_book::OrderBook;
using order_book::OrderId;
using order_book::Price;
using order_book::PriceLevel;
using order_book::RejectionReason;
using order_book::SequenceNum;
using order_book::Side;
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

// Same invariants as execution_test: one index entry per resting order, pointing at it,
// no leftover entries, no empty levels, and each level's aggregate matching its orders.
template <typename Levels>
void expectSideConsistent(const OrderBook& book, const Levels& levels, Side side,
                          std::size_t& liveOrders, const std::string& context) {
    for (const auto& [price, level] : levels) {
        expect(!level.isEmpty(), context + ": no empty price level should remain in the book");

        unsigned int remainingSum = 0;
        for (const Order& order : Access::orders(level)) {
            ++liveOrders;
            remainingSum += order.getRemainingQuantity();
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
    std::size_t liveOrders = 0;
    expectSideConsistent(book, Access::bids(book), Side::BUY, liveOrders, context);
    expectSideConsistent(book, Access::asks(book), Side::SELL, liveOrders, context);
    expect(Access::index(book).size() == liveOrders,
           context + ": the index should hold exactly one entry per live resting order");
}

// Everything a cancellation could disturb, including each order's address, so comparing
// two states also shows that no surviving order was moved or reinserted.
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
    unsigned int aggregateQuantity;
    std::vector<OrderState> orders;

    bool operator==(const LevelState&) const = default;
};

struct BookState {
    std::vector<LevelState> bids;
    std::vector<LevelState> asks;
    std::size_t indexSize;
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
                                         order.getRemainingQuantity(), order.getExecutionLevel(), &order});
        }
        captured.push_back(levelState);
    }
    return captured;
}

BookState capture(const OrderBook& book) {
    return {captureLevels(Access::bids(book)), captureLevels(Access::asks(book)), Access::index(book).size(),
            Access::nextSequenceNum(book), Access::nextTradeId(book)};
}

std::vector<LevelState> sideState(const BookState& state, Side side) {
    return side == Side::BUY ? state.bids : state.asks;
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

bool hasSide(const OrderBook& book, Side side) {
    return side == Side::BUY ? book.hasBids() : book.hasAsks();
}

Order bestOn(const OrderBook& book, Side side) {
    return side == Side::BUY ? book.getBestBid() : book.getBestAsk();
}

bool hasLevel(const OrderBook& book, Side side, Price price) {
    return side == Side::BUY ? Access::bids(book).contains(price) : Access::asks(book).contains(price);
}

const PriceLevel& levelAt(const OrderBook& book, Side side, Price price) {
    return side == Side::BUY ? Access::bids(book).at(price) : Access::asks(book).at(price);
}

std::vector<OrderId> idsAt(const OrderBook& book, Side side, Price price) {
    std::vector<OrderId> ids;
    for (const Order& order : Access::orders(levelAt(book, side, price))) {
        ids.push_back(order.getOrderId());
    }
    return ids;
}

// Bids rest around 100 and asks around 110, so the two sides never cross. bestPrice is
// the side's best level and worsePrice the next one behind it.
Price bestPrice(Side side) {
    return side == Side::BUY ? 100 : 110;
}

Price worsePrice(Side side) {
    return side == Side::BUY ? 99 : 111;
}

void expectCancelled(const CancellationResult& result, const std::string& context) {
    expect(result.success, context + ": the cancellation should succeed");
    expect(!result.rejectionReason.has_value(), context + ": a successful cancellation should carry no rejection reason");
}

void expectNotFound(const CancellationResult& result, const std::string& context) {
    expect(!result.success, context + ": the cancellation should be rejected");
    expect(result.rejectionReason == RejectionReason::ORDER_NOT_FOUND,
           context + ": the rejection reason should be ORDER_NOT_FOUND");
}

// The opposite side holds two levels, so isolation checks have something to disturb.
void seedOpposite(OrderBook& book, Side side) {
    const Side other = opposite(side);
    book.addOrder(Order(901, bestPrice(other), 7, 1, other));
    book.addOrder(Order(902, bestPrice(other), 8, 1, other));
    book.addOrder(Order(903, worsePrice(other), 9, 1, other));
}

void testCancelOnlyOrderAtPrice(Side side) {
    const std::string context = sideName(side) + " only order";
    OrderBook book{};
    book.addOrder(Order(1, bestPrice(side), 10, 1, side));
    seedOpposite(book, side);
    const BookState before = capture(book);

    expectCancelled(book.cancelOrder(1), context);
    expect(!hasSide(book, side), context + ": cancelling the only order should leave the side empty");
    expect(!hasLevel(book, side, bestPrice(side)), context + ": the emptied level should be erased");
    expect(!isIndexed(book, 1), context + ": the cancelled order should be unindexed");
    expectThrows([&book] { book.findActiveOrder(1); }, context + ": the cancelled order should not be found");
    expectThrows([&book, side] { bestOn(book, side); }, context + ": the empty side should have no best price");
    expect(sideState(capture(book), opposite(side)) == sideState(before, opposite(side)),
           context + ": the opposite side should be untouched");
    expectBookConsistent(book, context);
}

void testCancelUnknownId() {
    OrderBook empty{};
    const BookState emptyBefore = capture(empty);
    expectNotFound(empty.cancelOrder(12345), "unknown id on empty book");
    expect(capture(empty) == emptyBefore, "unknown id on empty book: the book should be unchanged");

    OrderBook book{};
    book.addOrder(Order(1, 100, 10, 1, Side::BUY));
    book.addOrder(Order(2, 100, 20, 1, Side::BUY));
    book.addOrder(Order(3, 110, 30, 1, Side::SELL));
    const BookState before = capture(book);

    expectNotFound(book.cancelOrder(12345), "unknown id");
    expect(capture(book) == before, "unknown id: the book, index, and counters should be unchanged");
    expectBookConsistent(book, "unknown id");
}

void testCancelTwice() {
    OrderBook book{};
    book.addOrder(Order(1, 100, 10, 1, Side::BUY));
    book.addOrder(Order(2, 100, 20, 1, Side::BUY));

    expectCancelled(book.cancelOrder(1), "cancel twice: first");
    const BookState afterFirst = capture(book);
    expectNotFound(book.cancelOrder(1), "cancel twice: second");
    expect(capture(book) == afterFirst, "cancel twice: the repeat rejection should leave the book unchanged");
    expect(idsAt(book, Side::BUY, 100) == std::vector<OrderId>{2}, "cancel twice: the other order should remain");
    expectBookConsistent(book, "cancel twice");
}

void testAggregateQuantity(Side side) {
    const std::string context = sideName(side) + " aggregate";
    const Price price = bestPrice(side);
    OrderBook book{};
    book.addOrder(Order(1, price, 50, 1, side));
    book.addOrder(Order(2, price, 30, 1, side));
    book.addOrder(Order(3, price, 60, 1, side));
    expect(levelAt(book, side, price).getAggregateQuantity() == 140, context + ": setup aggregate should be 140");

    expectCancelled(book.cancelOrder(2), context);
    expect(levelAt(book, side, price).getAggregateQuantity() == 110,
           context + ": cancelling 30 of 140 should leave 110");
    expect(levelAt(book, side, price).getOrdersQuantity() == 2, context + ": two orders should remain");
    expectBookConsistent(book, context);
}

void testCancelLastOrderAtLevel(Side side) {
    const std::string context = sideName(side) + " last order at level";
    OrderBook book{};
    book.addOrder(Order(1, bestPrice(side), 10, 1, side));
    book.addOrder(Order(2, bestPrice(side), 20, 1, side));
    book.addOrder(Order(3, worsePrice(side), 30, 1, side));

    expectCancelled(book.cancelOrder(1), context + ": first of two");
    expect(hasLevel(book, side, bestPrice(side)), context + ": a level should remain while it holds an order");

    expectCancelled(book.cancelOrder(2), context + ": last of two");
    expect(!hasLevel(book, side, bestPrice(side)), context + ": the level should be erased once empty");
    expect(hasLevel(book, side, worsePrice(side)), context + ": the other level should remain");
    expectBookConsistent(book, context);
}

void testBestPriceTransition(Side side) {
    const std::string context = sideName(side) + " best price transition";
    OrderBook book{};
    book.addOrder(Order(1, bestPrice(side), 10, 1, side));
    book.addOrder(Order(2, worsePrice(side), 20, 1, side));
    book.addOrder(Order(3, worsePrice(side), 30, 1, side));
    seedOpposite(book, side);
    expect(bestOn(book, side).getOrderId() == 1, context + ": setup best should be order 1");

    expectCancelled(book.cancelOrder(1), context);
    const Order best = bestOn(book, side);
    expect(best.getPriceTick() == worsePrice(side), context + ": the best price should move to the next level");
    expect(best.getOrderId() == 2, context + ": the oldest order at the new best level should be best");
    expect(bestOn(book, opposite(side)).getOrderId() == 901, context + ": the opposite best should not change");

    // A cancellation behind the best level leaves the best price alone.
    OrderBook deeper{};
    deeper.addOrder(Order(1, bestPrice(side), 10, 1, side));
    deeper.addOrder(Order(2, worsePrice(side), 20, 1, side));
    expectCancelled(deeper.cancelOrder(2), context + ": behind the best");
    expect(bestOn(deeper, side).getOrderId() == 1, context + ": cancelling behind the best should keep it");
    expectBookConsistent(book, context);
    expectBookConsistent(deeper, context + ": behind the best");
}

// Cancels one of [A, B, C, D] at a level and checks the survivors keep their relative
// order, their addresses (so their indexed iterators), and their sequence numbers.
void testCancelPosition(Side side, OrderId cancelled, const std::vector<OrderId>& survivors,
                        const std::string& position) {
    const std::string context = sideName(side) + " cancel " + position;
    const Price price = bestPrice(side);
    OrderBook book{};
    for (OrderId id : {1u, 2u, 3u, 4u}) {
        book.addOrder(Order(id, price, 10 * id, 1, side));
    }
    book.addOrder(Order(5, worsePrice(side), 5, 1, side));
    seedOpposite(book, side);
    const BookState before = capture(book);

    std::vector<const Order*> survivorAddresses;
    std::vector<SequenceNum> survivorSequences;
    for (OrderId id : survivors) {
        survivorAddresses.push_back(&Access::indexedOrder(book, id));
        survivorSequences.push_back(Access::indexedOrder(book, id).getSequenceNum());
    }

    expectCancelled(book.cancelOrder(cancelled), context);
    expect(idsAt(book, side, price) == survivors, context + ": surviving orders should keep their FIFO order");
    expect(!isIndexed(book, cancelled), context + ": the cancelled order should be unindexed");
    for (std::size_t i = 0; i < survivors.size(); ++i) {
        expect(isIndexed(book, survivors[i]), context + ": a surviving order should stay indexed");
        expect(&Access::indexedOrder(book, survivors[i]) == survivorAddresses[i],
               context + ": a surviving order's iterator should still point at the same element");
        expect(Access::indexedOrder(book, survivors[i]).getSequenceNum() == survivorSequences[i],
               context + ": a surviving order should keep its sequence number");
    }
    expect(bestOn(book, side).getOrderId() == survivors.front(),
           context + ": the best order should be the oldest survivor");

    const BookState after = capture(book);
    expect(sideState(after, opposite(side)) == sideState(before, opposite(side)),
           context + ": the opposite side should be untouched");
    const std::vector<LevelState> sameSideBefore = sideState(before, side);
    const std::vector<LevelState> sameSideAfter = sideState(after, side);
    expect(sameSideAfter.size() == 2 && sameSideAfter.back() == sameSideBefore.back(),
           context + ": the other level on the same side should be untouched");
    expect(after.nextSequenceNum == before.nextSequenceNum && after.nextTradeId == before.nextTradeId,
           context + ": cancelling should consume no sequence number or trade id");
    expectBookConsistent(book, context);
}

void testCancelPartiallyFilled(Side side) {
    const std::string context = sideName(side) + " partially filled";
    const Price price = bestPrice(side);
    OrderBook book{};
    book.addOrder(Order(1, price, 100, 1, side));
    book.addOrder(Order(2, price, 25, 1, side));

    // Fill 40 of order 1, leaving 60 resting.
    const auto trades = book.submitOrder(Order(50, price, 40, 1, opposite(side))).getGeneratedTrades();
    expect(trades.size() == 1, context + ": setup should execute once");
    expect(book.findActiveOrder(1).getRemainingQuantity() == 60, context + ": setup should leave 60 on order 1");
    expect(book.findActiveOrder(1).getExecutionLevel() == FillStatus::PARTIAL,
           context + ": setup should leave order 1 partially filled");
    expect(levelAt(book, side, price).getAggregateQuantity() == 85, context + ": setup aggregate should be 85");

    expectCancelled(book.cancelOrder(1), context);
    expect(levelAt(book, side, price).getAggregateQuantity() == 25,
           context + ": cancelling should remove the 60 remaining, not the 100 submitted");
    expect(idsAt(book, side, price) == std::vector<OrderId>{2}, context + ": the other order should remain");
    expect(!isIndexed(book, 1), context + ": the cancelled order should be unindexed");
    expectBookConsistent(book, context);

    // The incoming order filled completely and never rested, so there is nothing to cancel.
    const BookState before = capture(book);
    expectNotFound(book.cancelOrder(50), context + ": a fully filled incoming order");
    expect(capture(book) == before, context + ": rejecting a never-rested order should leave the book unchanged");
}

void testCancelledOrderIsNotMatchedAndIteratorsSurvive(Side side) {
    const std::string context = sideName(side) + " iterator survival";
    const Price price = bestPrice(side);
    OrderBook book{};
    for (OrderId id : {1u, 2u, 3u, 4u}) {
        book.addOrder(Order(id, price, 10, 1, side));
    }

    expectCancelled(book.cancelOrder(2), context);

    // Matching walks the level through the surviving iterators: A, C, D, skipping B.
    const auto trades = book.submitOrder(Order(60, price, 30, 1, opposite(side))).getGeneratedTrades();
    expect(trades.size() == 3, context + ": the sweep should execute against each survivor");
    expect(trades[0].getRestingId() == 1 && trades[1].getRestingId() == 3 && trades[2].getRestingId() == 4,
           context + ": the sweep should fill survivors in FIFO order and never the cancelled order");
    expect(!hasSide(book, side), context + ": the sweep should empty the side");
    expect(Access::index(book).empty(), context + ": no index entries should remain");
    expectBookConsistent(book, context);
}

void testCancelledIdCanBeReused() {
    OrderBook book{};
    book.addOrder(Order(1, 100, 10, 1, Side::BUY));
    book.addOrder(Order(2, 100, 20, 1, Side::BUY));
    expectCancelled(book.cancelOrder(1), "reuse");

    book.addOrder(Order(1, 100, 5, 1, Side::BUY));
    expect(idsAt(book, Side::BUY, 100) == std::vector<OrderId>{2, 1},
           "reuse: a re-added id should join the back of the queue");
    expect(book.findActiveOrder(1).getRemainingQuantity() == 5, "reuse: the index should hold the new order");
    expectBookConsistent(book, "reuse");
}

// Cancels a mixed book in an arbitrary order, checking consistency after each step.
void testIndexBookConsistencyThroughout() {
    OrderBook book{};
    book.addOrder(Order(1, 100, 10, 1, Side::BUY));
    book.addOrder(Order(2, 100, 20, 1, Side::BUY));
    book.addOrder(Order(3, 99, 30, 1, Side::BUY));
    book.addOrder(Order(4, 110, 40, 1, Side::SELL));
    book.addOrder(Order(5, 111, 50, 1, Side::SELL));
    book.addOrder(Order(6, 111, 60, 1, Side::SELL));
    book.submitOrder(Order(7, 100, 5, 1, Side::SELL));  // partially fills order 1

    for (OrderId id : {5u, 1u, 4u, 3u, 6u, 2u}) {
        const std::string context = "consistency: cancel " + std::to_string(id);
        expectCancelled(book.cancelOrder(id), context);
        expect(!isIndexed(book, id), context + ": the cancelled order should be unindexed");
        expectBookConsistent(book, context);
    }
    expect(!book.hasBids() && !book.hasAsks(), "consistency: every order cancelled should empty the book");
    expect(Access::index(book).empty(), "consistency: the index should be empty");
}

}  // namespace

int main() {
    for (Side side : {Side::BUY, Side::SELL}) {
        testCancelOnlyOrderAtPrice(side);
        testAggregateQuantity(side);
        testCancelLastOrderAtLevel(side);
        testBestPriceTransition(side);
        testCancelPosition(side, 1, {2, 3, 4}, "oldest");
        testCancelPosition(side, 2, {1, 3, 4}, "middle");
        testCancelPosition(side, 4, {1, 2, 3}, "newest");
        testCancelPartiallyFilled(side);
        testCancelledOrderIsNotMatchedAndIteratorsSurvive(side);
    }
    testCancelUnknownId();
    testCancelTwice();
    testCancelledIdCanBeReused();
    testIndexBookConsistencyThroughout();

    std::cout << "cancellation tests passed" << std::endl;
    return EXIT_SUCCESS;
}
