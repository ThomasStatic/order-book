#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <list>
#include <stdexcept>
#include <string>
#include <utility>

#include "order_book/order.hpp"
#include "order_book/order_book.hpp"

namespace order_book {

// Befriended by OrderBook and PriceLevel so these tests can drive one execution and
// inspect internals without widening the public API.
struct OrderBookTestAccess {
    static Trade executeOnce(OrderBook& book, Order& incoming) { return book.executeOnce(incoming); }
    static void restOrder(OrderBook& book, const Order& order) { book.restOrder(order); }
    // Plants an empty level, which no public operation leaves behind, to exercise the guard.
    static void insertEmptyAsk(OrderBook& book, Price price) { book.asks.try_emplace(price, price, Side::SELL); }

    static const auto& index(const OrderBook& book) { return book.orderIndex; }
    static const auto& bids(const OrderBook& book) { return book.bids; }
    static const auto& asks(const OrderBook& book) { return book.asks; }
    static TradeId nextTradeId(const OrderBook& book) { return book.nextTradeId; }
    // The resting order itself (findActiveOrder only returns a copy), to check identity.
    static const Order& indexedOrder(const OrderBook& book, OrderId orderId) { return *book.orderIndex.at(orderId).itr; }
    // Bypasses the read-only findActiveOrder to corrupt a resting order on purpose.
    static Order& restingOrder(OrderBook& book, OrderId orderId) { return *book.orderIndex.at(orderId).itr; }

    static const order_book::OrderQueue& orders(const PriceLevel& level) { return level.orders; }
    static OrderFill fillOldestOrder(PriceLevel& level, unsigned int quantity) {
        return level.fillOldestOrder(quantity);
    }
};

}  // namespace order_book

namespace {

using Access = order_book::OrderBookTestAccess;
using order_book::FillStatus;
using order_book::Order;
using order_book::OrderBook;
using order_book::PriceLevel;
using order_book::RejectionReason;
using order_book::Side;
using order_book::Trade;

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

// Every live resting order has exactly one index entry whose iterator points at that
// exact list element (and agrees on price and side), no index entry is left over, and
// every level's cached aggregate matches the sum of its orders' remaining quantities.
template <typename Levels>
void expectSideConsistent(const OrderBook& book, const Levels& levels, Side side,
                          std::size_t& liveOrders, const std::string& context) {
    for (const auto& [price, level] : levels) {
        expect(!level.isEmpty(), context + ": no empty price level should remain in the book");

        unsigned int remainingSum = 0;
        for (const Order& order : Access::orders(level)) {
            ++liveOrders;
            remainingSum += order.getRemainingQuantity();
            expect(order.getRemainingQuantity() > 0, context + ": no fully filled order should rest in a level");

            auto indexItr = Access::index(book).find(order.getOrderId());
            expect(indexItr != Access::index(book).end(), context + ": every resting order should be indexed");
            expect(&*indexItr->second.itr == &order,
                   context + ": an index entry should point at its resting order's list element");
            expect(indexItr->second.tickPrice == price, context + ": an index entry should record the level price");
            expect(indexItr->second.side == side, context + ": an index entry should record the resting side");
        }
        expect(level.getAggregateQuantity() == remainingSum,
               context + ": a level's aggregate quantity should equal its orders' remaining quantities");
    }
}

void expectIndexConsistent(const OrderBook& book, const std::string& context) {
    std::size_t liveOrders = 0;
    expectSideConsistent(book, Access::bids(book), Side::BUY, liveOrders, context);
    expectSideConsistent(book, Access::asks(book), Side::SELL, liveOrders, context);
    expect(Access::index(book).size() == liveOrders,
           context + ": the index should hold exactly one entry per live resting order");
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

// Fill tests rest liquidity at 100 and run once per incoming side; this limit price
// crosses that level with room to spare.
unsigned int crossingPrice(Side incomingSide) {
    return incomingSide == Side::BUY ? 101 : 99;
}

bool hasSide(const OrderBook& book, Side side) {
    return side == Side::BUY ? book.hasBids() : book.hasAsks();
}

const PriceLevel& levelAt(const OrderBook& book, Side side, unsigned int price) {
    return side == Side::BUY ? Access::bids(book).at(price) : Access::asks(book).at(price);
}

void testExactFill(Side incomingSide) {
    const std::string context = "exact fill (incoming " + sideName(incomingSide) + ")";
    const Side restingSide = opposite(incomingSide);
    OrderBook book{};
    book.addOrder(Order(1, 100, 10, 1, restingSide));
    Order incoming(2, 100, 10, 2, incomingSide);

    const Trade trade = Access::executeOnce(book, incoming);

    expect(trade.getExecutionQuantity() == 10, context + ": execution quantity should equal both quantities");
    expect(trade.getExecutionPrice() == 100, context + ": trade should execute at the resting price");
    expect(incoming.getRemainingQuantity() == 0, context + ": incoming order should have nothing left");
    expect(incoming.getExecutionLevel() == FillStatus::FILLED, context + ": incoming order should be FILLED");
    expect(!isIndexed(book, 1), context + ": the fully filled resting order should be unindexed");
    expect(!hasSide(book, restingSide), context + ": the emptied price level should be erased");
    expectIndexConsistent(book, context);
}

void testPartialIncomingFill(Side incomingSide) {
    const std::string context = "partial incoming (incoming " + sideName(incomingSide) + ")";
    const Side restingSide = opposite(incomingSide);
    OrderBook book{};
    book.addOrder(Order(1, 100, 4, 1, restingSide));
    book.addOrder(Order(3, 100, 6, 2, restingSide));
    Order incoming(2, crossingPrice(incomingSide), 10, 3, incomingSide);

    const Trade trade = Access::executeOnce(book, incoming);

    expect(trade.getExecutionQuantity() == 4, context + ": execution should be capped by the resting order");
    expect(trade.getRestingId() == 1, context + ": execution should hit the oldest resting order");
    expect(incoming.getRemainingQuantity() == 6, context + ": incoming should keep the unexecuted quantity");
    expect(incoming.getExecutionLevel() == FillStatus::PARTIAL, context + ": incoming should be PARTIAL");
    expect(!isIndexed(book, 1), context + ": the filled resting order should be unindexed");

    // One execution touches exactly one resting order.
    const PriceLevel& level = levelAt(book, restingSide, 100);
    const Order& next = level.getOldestOrder();
    expect(next.getOrderId() == 3, context + ": the next order should now be at the front of the level");
    expect(next.getRemainingQuantity() == 6 && next.getExecutionLevel() == FillStatus::NONE,
           context + ": one execution should not touch the next resting order");
    expect(level.getAggregateQuantity() == 6, context + ": level aggregate should drop by 4");
    expect(level.getOrdersQuantity() == 1, context + ": level should hold one order");
    expectIndexConsistent(book, context);
}

void testPartialRestingFill(Side incomingSide) {
    const std::string context = "partial resting (incoming " + sideName(incomingSide) + ")";
    const Side restingSide = opposite(incomingSide);
    OrderBook book{};
    book.addOrder(Order(1, 100, 10, 1, restingSide));
    book.addOrder(Order(3, 100, 5, 2, restingSide));
    const Order* restingBefore = &Access::indexedOrder(book, 1);
    const Order* behindBefore = &Access::indexedOrder(book, 3);
    Order incoming(2, 100, 4, 3, incomingSide);

    const Trade trade = Access::executeOnce(book, incoming);

    expect(trade.getExecutionQuantity() == 4, context + ": execution should be capped by the incoming order");
    expect(incoming.getRemainingQuantity() == 0, context + ": incoming should be fully executed");
    expect(incoming.getExecutionLevel() == FillStatus::FILLED, context + ": incoming should be FILLED");

    const Order& resting = Access::indexedOrder(book, 1);
    expect(&resting == restingBefore, context + ": the resting order should keep its original index entry");
    expect(resting.getRemainingQuantity() == 6, context + ": the resting order should drop to 6");
    expect(resting.getExecutionLevel() == FillStatus::PARTIAL, context + ": the resting order should be PARTIAL");

    const PriceLevel& level = levelAt(book, restingSide, 100);
    expect(level.getOldestOrder().getOrderId() == 1, context + ": the resting order should keep its FIFO position");
    expect(&Access::indexedOrder(book, 3) == behindBefore, context + ": orders behind should keep their entries");
    expect(level.getAggregateQuantity() == 11, context + ": level aggregate should drop from 15 to 11");
    expect(level.getOrdersQuantity() == 2, context + ": no order should leave the level");
    expectIndexConsistent(book, context);
}

void testTradePrice() {
    OrderBook buyBook{};
    buyBook.addOrder(Order(1, 100, 5, 1, Side::SELL));
    Order aggressiveBuy(2, 105, 5, 2, Side::BUY);
    expect(Access::executeOnce(buyBook, aggressiveBuy).getExecutionPrice() == 100,
           "trade price: an incoming buy above the ask should execute at the resting ask price");

    OrderBook sellBook{};
    sellBook.addOrder(Order(1, 100, 5, 1, Side::BUY));
    Order aggressiveSell(2, 95, 5, 2, Side::SELL);
    expect(Access::executeOnce(sellBook, aggressiveSell).getExecutionPrice() == 100,
           "trade price: an incoming sell below the bid should execute at the resting bid price");

    OrderBook levelsBook{};
    levelsBook.addOrder(Order(1, 101, 2, 1, Side::SELL));
    levelsBook.addOrder(Order(3, 100, 2, 2, Side::SELL));
    Order sweep(2, 101, 4, 3, Side::BUY);
    expect(Access::executeOnce(levelsBook, sweep).getExecutionPrice() == 100,
           "trade price: execution should take the best opposing level first");
    expect(Access::executeOnce(levelsBook, sweep).getExecutionPrice() == 101,
           "trade price: the next execution should use the next level's price");
}

void testTradeQuantity() {
    OrderBook book{};
    book.addOrder(Order(1, 100, 3, 1, Side::SELL));
    book.addOrder(Order(3, 100, 8, 2, Side::SELL));
    Order incoming(2, 100, 7, 3, Side::BUY);

    const Trade first = Access::executeOnce(book, incoming);
    const Trade second = Access::executeOnce(book, incoming);

    expect(first.getExecutionQuantity() == 3, "trade quantity: first execution should be min(7, 3)");
    expect(second.getExecutionQuantity() == 4, "trade quantity: second execution should be min(4, 8)");
    expect(incoming.getRemainingQuantity() == 0, "trade quantity: executions should sum to the incoming quantity");
    expect(book.getBestAsk().getRemainingQuantity() == 4, "trade quantity: resting remainder should be 8 - 4");
    expectThrows([&book, &incoming] { Access::executeOnce(book, incoming); },
                 "trade quantity: executing an exhausted incoming order should throw");
    expectIndexConsistent(book, "trade quantity");
}

void testMakerAndTakerIds() {
    OrderBook buyBook{};
    buyBook.addOrder(Order(11, 100, 5, 1, Side::SELL));
    Order takerBuy(22, 100, 5, 2, Side::BUY);
    const Trade buyTrade = Access::executeOnce(buyBook, takerBuy);
    expect(buyTrade.getRestingId() == 11, "ids: maker should be the resting sell");
    expect(buyTrade.getIncomingId() == 22, "ids: taker should be the incoming buy");
    expect(buyTrade.getIncomingSide() == Side::BUY, "ids: incoming side should be BUY");
    expect(buyTrade.getBuyOrderId() == 22 && buyTrade.getSellOrderId() == 11,
           "ids: an incoming buy should be the buy order and the maker the sell order");

    OrderBook sellBook{};
    sellBook.addOrder(Order(33, 100, 5, 1, Side::BUY));
    Order takerSell(44, 100, 5, 2, Side::SELL);
    const Trade sellTrade = Access::executeOnce(sellBook, takerSell);
    expect(sellTrade.getRestingId() == 33, "ids: maker should be the resting buy");
    expect(sellTrade.getIncomingId() == 44, "ids: taker should be the incoming sell");
    expect(sellTrade.getIncomingSide() == Side::SELL, "ids: incoming side should be SELL");
    expect(sellTrade.getBuyOrderId() == 33 && sellTrade.getSellOrderId() == 44,
           "ids: an incoming sell should be the sell order and the maker the buy order");

    OrderBook idBook{};
    idBook.addOrder(Order(1, 100, 1, 1, Side::SELL));
    idBook.addOrder(Order(3, 100, 1, 2, Side::SELL));
    Order incoming(2, 100, 2, 3, Side::BUY);
    expect(Access::executeOnce(idBook, incoming).getTradeId() == 1, "ids: the first execution should get trade ID 1");
    expect(Access::executeOnce(idBook, incoming).getTradeId() == 2, "ids: each execution should get the next trade ID");
}

void testAggregateQuantityUpdates() {
    OrderBook book{};
    book.addOrder(Order(1, 200, 5, 1, Side::BUY));
    book.addOrder(Order(3, 200, 7, 2, Side::BUY));
    book.addOrder(Order(5, 199, 4, 3, Side::BUY));
    expect(Access::bids(book).at(200).getAggregateQuantity() == 12, "aggregate: starting level aggregate should be 12");

    Order incoming(2, 199, 9, 4, Side::SELL);
    Access::executeOnce(book, incoming);
    expect(Access::bids(book).at(200).getAggregateQuantity() == 7, "aggregate: should drop by the 5 executed (12 -> 7)");
    expectIndexConsistent(book, "aggregate after first execution");

    Access::executeOnce(book, incoming);
    expect(Access::bids(book).at(200).getAggregateQuantity() == 3, "aggregate: should drop by the 4 executed (7 -> 3)");
    expect(Access::bids(book).at(199).getAggregateQuantity() == 4, "aggregate: other levels should be unaffected");
    expectIndexConsistent(book, "aggregate after second execution");

    Order drain(4, 199, 3, 5, Side::SELL);
    Access::executeOnce(book, drain);
    expect(Access::bids(book).find(200) == Access::bids(book).end(), "aggregate: a level drained to zero should be erased");
    expect(book.getBestBid().getOrderId() == 5, "aggregate: the next level should become best");
    expectIndexConsistent(book, "aggregate after draining a level");
}

void testActiveIndexCleanup() {
    OrderBook book{};
    book.addOrder(Order(1, 100, 2, 1, Side::SELL));
    book.addOrder(Order(3, 100, 3, 2, Side::SELL));
    book.addOrder(Order(5, 100, 4, 3, Side::SELL));
    book.addOrder(Order(7, 90, 4, 4, Side::BUY));
    const Order* thirdBefore = &Access::indexedOrder(book, 5);

    Order incoming(2, 100, 6, 5, Side::BUY);
    Access::executeOnce(book, incoming);  // fills 1 fully
    expect(!isIndexed(book, 1), "cleanup: a fully filled resting order should be unindexed");
    expect(book.cancelOrder(1).getRejectionReason() == RejectionReason::ORDER_NOT_FOUND,
           "cleanup: cancelling a fully filled order should be rejected rather than use a stale iterator");
    expectIndexConsistent(book, "cleanup after first fill");

    Access::executeOnce(book, incoming);  // fills 3 fully
    Access::executeOnce(book, incoming);  // partially fills 5 (1 of 4)
    expect(!isIndexed(book, 3), "cleanup: the second fully filled order should be unindexed");
    expect(isIndexed(book, 5), "cleanup: a partially filled order should stay indexed");
    expect(&Access::indexedOrder(book, 5) == thirdBefore,
           "cleanup: popping orders ahead should not invalidate a later order's iterator");
    expect(isIndexed(book, 7), "cleanup: orders on the other side should be untouched");
    expect(!isIndexed(book, 2), "cleanup: an executing incoming order should never be indexed");
    expectIndexConsistent(book, "cleanup after partial fill");

    expect(book.cancelOrder(5).isSuccess(), "cleanup: cancelling the partially filled order should succeed");
    expect(!book.hasAsks(), "cleanup: the partially filled order should still be cancellable via its entry");
    expectIndexConsistent(book, "cleanup after cancel");
}

void testExecutionPreconditions() {
    OrderBook empty{};
    Order lonelyBuy(1, 100, 5, 1, Side::BUY);
    expectThrows([&empty, &lonelyBuy] { Access::executeOnce(empty, lonelyBuy); },
                 "preconditions: executing into an empty opposing side should throw");

    OrderBook book{};
    book.addOrder(Order(1, 101, 5, 1, Side::SELL));
    Order nonCrossing(2, 100, 5, 2, Side::BUY);
    expectThrows([&book, &nonCrossing] { Access::executeOnce(book, nonCrossing); },
                 "preconditions: executing a non-crossing order should throw");
    expect(nonCrossing.getRemainingQuantity() == 5, "preconditions: a rejected execution should not fill the incoming order");
    expect(book.getBestAsk().getRemainingQuantity() == 5, "preconditions: a rejected execution should not fill the resting order");
    expect(Access::nextTradeId(book) == 1, "preconditions: a rejected execution should not consume a trade ID");
    expectIndexConsistent(book, "preconditions");
}

// A throw from inside an execution must leave the book exactly as it was: no order
// filled or popped, no dangling index entry, no empty level, no trade ID consumed.
// Checks that never follow an index entry run first, so a dangling entry is reported
// as a failure rather than tripping the debug-iterator assertion.
void expectUntouched(const OrderBook& book, const Order& incoming, const std::string& context) {
    expect(incoming.getRemainingQuantity() == 5, context + ": the incoming order should not be filled");
    expect(Access::nextTradeId(book) == 1, context + ": no trade ID should be consumed");
    expect(levelAt(book, Side::SELL, 100).getAggregateQuantity() == 5, context + ": the level aggregate should not change");
    expectIndexConsistent(book, context);
    expect(isIndexed(book, 1), context + ": the resting order should stay indexed");
    expect(book.findActiveOrder(1).getRemainingQuantity() == 5, context + ": the resting order should not be filled");
}

void testRejectedExecutionLeavesBookUntouched() {
    OrderBook book{};
    book.addOrder(Order(1, 100, 5, 1, Side::SELL));

    // An order marked FILLED while still holding quantity: filling it would throw.
    Order inactiveIncoming(2, 100, 5, 2, Side::BUY);
    inactiveIncoming.setExecutionLevel(FillStatus::FILLED);
    expectThrows([&book, &inactiveIncoming] { Access::executeOnce(book, inactiveIncoming); },
                 "rejected execution: an inactive incoming order should throw");
    expectUntouched(book, inactiveIncoming, "rejected execution (inactive incoming)");

    expect(book.submitOrder(inactiveIncoming).getRejectionReason() == RejectionReason::ORDER_NOT_ACTIVE,
           "rejected execution: submitting an inactive incoming order should be rejected");
    expectUntouched(book, inactiveIncoming, "rejected submission (inactive incoming)");

    // The public API can no longer rest an inactive order (addOrder rejects one and
    // findActiveOrder is read-only), so corrupt one directly to exercise the guard.
    Access::restingOrder(book, 1).setExecutionLevel(FillStatus::FILLED);
    Order incoming(3, 100, 5, 3, Side::BUY);
    expectThrows([&book, &incoming] { Access::executeOnce(book, incoming); },
                 "rejected execution: an inactive resting order should throw");
    // Undo the corruption so the consistency checks judge only what the execution did.
    Access::restingOrder(book, 1).setExecutionLevel(FillStatus::NONE);
    expectUntouched(book, incoming, "rejected execution (inactive resting)");
}

void testRestOrderRejectsIndexedId() {
    OrderBook book{};
    book.addOrder(Order(1, 100, 5, 1, Side::SELL));

    // restOrder skips validation, so it must still refuse to rest an order it cannot index.
    expectThrows([&book] { Access::restOrder(book, Order(1, 100, 7, 2, Side::SELL)); },
                 "restOrder: an already indexed ID at an existing level should throw");
    const PriceLevel& level = levelAt(book, Side::SELL, 100);
    expect(level.getOrdersQuantity() == 1 && level.getAggregateQuantity() == 5,
           "restOrder: the existing level should be left as it was");
    expect(book.findActiveOrder(1).getRemainingQuantity() == 5, "restOrder: the original entry should be kept");
    expectIndexConsistent(book, "restOrder (existing level)");

    expectThrows([&book] { Access::restOrder(book, Order(1, 101, 7, 2, Side::SELL)); },
                 "restOrder: an already indexed ID at a new level should throw");
    expect(Access::asks(book).find(101) == Access::asks(book).end(), "restOrder: no new level should be left behind");
    expectIndexConsistent(book, "restOrder (new level)");
}

void testEmptyBestLevelIsRejected() {
    OrderBook book{};
    book.addOrder(Order(1, 101, 5, 1, Side::SELL));
    Access::insertEmptyAsk(book, 100);
    Order incoming(2, 101, 5, 2, Side::BUY);

    expectThrows([&book, &incoming] { Access::executeOnce(book, incoming); },
                 "empty level: executing against an empty best level should throw");
    expect(incoming.getRemainingQuantity() == 5, "empty level: the incoming order should not be filled");
    expect(Access::nextTradeId(book) == 1, "empty level: no trade ID should be consumed");
    expect(book.findActiveOrder(1).getRemainingQuantity() == 5, "empty level: the order behind should not be filled");
}

// Only a corrupted order makes a fill throw partway through, and the order is still corrupted
// when consumeLevel returns. With automatic invariant checks on, its boundary rightly aborts
// there, so this runs only in builds without them (e.g. Release).
void testConsumeLevelUnindexesEachOrderAsItIsFilled() {
    if constexpr (!order_book::invariantChecksAfterCommands) {
        OrderBook book{};
        book.addOrder(Order(1, 100, 5, 1, Side::SELL));
        book.addOrder(Order(2, 100, 5, 2, Side::SELL));
        // Corrupt the second order so its fill throws after the first has left the level.
        Access::restingOrder(book, 2).setExecutionLevel(FillStatus::FILLED);

        expectThrows([&book] { book.consumeLevel(Side::SELL, 100, 10); },
                     "consumeLevel: filling a corrupted order should throw");
        expect(!isIndexed(book, 1), "consumeLevel: an order filled before the throw should already be unindexed");
        expect(isIndexed(book, 2) && levelAt(book, Side::SELL, 100).getOrdersQuantity() == 1,
               "consumeLevel: the order that threw should remain resting and indexed");
        // Undo the corruption: the throw should have left everything else sound.
        Access::restingOrder(book, 2).setExecutionLevel(FillStatus::NONE);
        expectIndexConsistent(book, "consumeLevel after a throw");
    }
}

void testMovedBookKeepsValidIndex() {
    OrderBook original{};
    original.addOrder(Order(1, 100, 5, 1, Side::SELL));
    original.addOrder(Order(2, 100, 5, 2, Side::SELL));
    original.addOrder(Order(3, 99, 5, 3, Side::BUY));
    const Order* firstBefore = &Access::indexedOrder(original, 1);

    OrderBook moved(std::move(original));
    expect(&Access::indexedOrder(moved, 1) == firstBefore, "move: index entries should still point at the same orders");
    expectIndexConsistent(moved, "move construction");

    expect(moved.cancelOrder(1).isSuccess(), "move: the moved index should cancel an order");
    Order incoming(4, 100, 2, 4, Side::BUY);
    Access::executeOnce(moved, incoming);
    expectIndexConsistent(moved, "move construction then mutation");

    OrderBook assigned{};
    assigned.addOrder(Order(9, 50, 1, 5, Side::BUY));
    assigned = std::move(moved);
    expect(!isIndexed(assigned, 9), "move assignment: the target's previous orders should be gone");
    expect(assigned.cancelOrder(2).isSuccess() && assigned.cancelOrder(3).isSuccess(),
           "move assignment: cancelling each moved order should succeed");
    expect(!assigned.hasBids() && !assigned.hasAsks(), "move assignment: the moved index should still cancel every order");
    expectIndexConsistent(assigned, "move assignment then removal");
}

void testFillOldestOrderGuards() {
    PriceLevel level(100, Side::SELL, Order(1, 100, 5, 1, Side::SELL));
    expectThrows([&level] { Access::fillOldestOrder(level, 0); },
                 "fill guards: filling zero quantity should throw");
    expect(level.getAggregateQuantity() == 5 && level.getOldestOrder().getRemainingQuantity() == 5,
           "fill guards: a rejected zero fill should not change the level");

    Access::fillOldestOrder(level, 5);
    expect(level.isEmpty(), "fill guards: a full fill should pop the only order");
    expectThrows([&level] { Access::fillOldestOrder(level, 1); },
                 "fill guards: filling an empty level should throw");
}

}  // namespace

int main() {
    for (const Side incomingSide : {Side::BUY, Side::SELL}) {
        testExactFill(incomingSide);
        testPartialIncomingFill(incomingSide);
        testPartialRestingFill(incomingSide);
    }
    testTradePrice();
    testTradeQuantity();
    testMakerAndTakerIds();
    testAggregateQuantityUpdates();
    testActiveIndexCleanup();
    testExecutionPreconditions();
    testRejectedExecutionLeavesBookUntouched();
    testFillOldestOrderGuards();
    testRestOrderRejectsIndexedId();
    testEmptyBestLevelIsRejected();
    testConsumeLevelUnindexesEachOrderAsItIsFilled();
    testMovedBookKeepsValidIndex();

    std::cout << "execution tests passed" << std::endl;
    return EXIT_SUCCESS;
}
