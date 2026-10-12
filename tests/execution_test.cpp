#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <list>
#include <stdexcept>
#include <string>

#include "order_book/order.hpp"
#include "order_book/order_book.hpp"

namespace order_book {

// Befriended by OrderBook and PriceLevel so these tests can drive one execution and
// inspect internals without widening the public API.
struct OrderBookTestAccess {
    static Trade executeOnce(OrderBook& book, Order& incoming) { return book.executeOnce(incoming); }

    static const auto& index(const OrderBook& book) { return book.orderIndex; }
    static const auto& bids(const OrderBook& book) { return book.bids; }
    static const auto& asks(const OrderBook& book) { return book.asks; }
    static TradeId nextTradeId(const OrderBook& book) { return book.nextTradeId; }

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
}

bool isIndexed(const OrderBook& book, unsigned int orderId) {
    return Access::index(book).find(orderId) != Access::index(book).end();
}

void testExactFill() {
    OrderBook book{};
    book.addOrder(Order(1, 100, 10, 1, Side::SELL));
    Order incoming(2, 100, 10, 2, Side::BUY);

    const Trade trade = Access::executeOnce(book, incoming);

    expect(trade.getExecutionQuantity() == 10, "exact fill: execution quantity should equal both quantities");
    expect(trade.getExecutionPrice() == 100, "exact fill: trade should execute at the resting price");
    expect(incoming.getRemainingQuantity() == 0, "exact fill: incoming order should have nothing left");
    expect(incoming.getExecutionLevel() == FillStatus::FILLED, "exact fill: incoming order should be FILLED");
    expect(!isIndexed(book, 1), "exact fill: the fully filled resting order should be unindexed");
    expect(!book.hasAsks(), "exact fill: the emptied price level should be erased");
    expectIndexConsistent(book, "exact fill");
}

void testPartialIncomingFill() {
    OrderBook book{};
    book.addOrder(Order(1, 100, 4, 1, Side::SELL));
    book.addOrder(Order(3, 100, 6, 2, Side::SELL));
    Order incoming(2, 101, 10, 3, Side::BUY);

    const Trade trade = Access::executeOnce(book, incoming);

    expect(trade.getExecutionQuantity() == 4, "partial incoming: execution should be capped by the resting order");
    expect(trade.getRestingId() == 1, "partial incoming: execution should hit the oldest resting order");
    expect(incoming.getRemainingQuantity() == 6, "partial incoming: incoming should keep the unexecuted quantity");
    expect(incoming.getExecutionLevel() == FillStatus::PARTIAL, "partial incoming: incoming should be PARTIAL");
    expect(!isIndexed(book, 1), "partial incoming: the filled resting order should be unindexed");

    // One execution touches exactly one resting order.
    const Order& next = Access::asks(book).at(100).getOldestOrder();
    expect(next.getOrderId() == 3, "partial incoming: the next order should now be at the front of the level");
    expect(next.getRemainingQuantity() == 6 && next.getExecutionLevel() == FillStatus::NONE,
           "partial incoming: one execution should not touch the next resting order");
    expect(Access::asks(book).at(100).getAggregateQuantity() == 6, "partial incoming: level aggregate should drop by 4");
    expect(Access::asks(book).at(100).getOrdersQuantity() == 1, "partial incoming: level should hold one order");
    expectIndexConsistent(book, "partial incoming");
}

void testPartialRestingFill() {
    OrderBook book{};
    book.addOrder(Order(1, 100, 10, 1, Side::SELL));
    book.addOrder(Order(3, 100, 5, 2, Side::SELL));
    const Order* restingBefore = &*book.findActiveOrder(1).itr;
    const Order* behindBefore = &*book.findActiveOrder(3).itr;
    Order incoming(2, 100, 4, 3, Side::BUY);

    const Trade trade = Access::executeOnce(book, incoming);

    expect(trade.getExecutionQuantity() == 4, "partial resting: execution should be capped by the incoming order");
    expect(incoming.getRemainingQuantity() == 0, "partial resting: incoming should be fully executed");
    expect(incoming.getExecutionLevel() == FillStatus::FILLED, "partial resting: incoming should be FILLED");

    const auto location = book.findActiveOrder(1);
    expect(&*location.itr == restingBefore, "partial resting: the resting order should keep its original index entry");
    expect(location.itr->getRemainingQuantity() == 6, "partial resting: the resting order should drop to 6");
    expect(location.itr->getExecutionLevel() == FillStatus::PARTIAL, "partial resting: the resting order should be PARTIAL");
    expect(book.getBestAsk().getOrderId() == 1, "partial resting: the resting order should keep its FIFO position");
    expect(&*book.findActiveOrder(3).itr == behindBefore, "partial resting: orders behind should keep their entries");
    expect(Access::asks(book).at(100).getAggregateQuantity() == 11, "partial resting: level aggregate should drop from 15 to 11");
    expect(Access::asks(book).at(100).getOrdersQuantity() == 2, "partial resting: no order should leave the level");
    expectIndexConsistent(book, "partial resting");
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
    const Order* thirdBefore = &*book.findActiveOrder(5).itr;

    Order incoming(2, 100, 6, 5, Side::BUY);
    Access::executeOnce(book, incoming);  // fills 1 fully
    expect(!isIndexed(book, 1), "cleanup: a fully filled resting order should be unindexed");
    expectThrows([&book] { book.removeOrder(1); },
                 "cleanup: cancelling a fully filled order should throw rather than use a stale iterator");
    expectIndexConsistent(book, "cleanup after first fill");

    Access::executeOnce(book, incoming);  // fills 3 fully
    Access::executeOnce(book, incoming);  // partially fills 5 (1 of 4)
    expect(!isIndexed(book, 3), "cleanup: the second fully filled order should be unindexed");
    expect(isIndexed(book, 5), "cleanup: a partially filled order should stay indexed");
    expect(&*book.findActiveOrder(5).itr == thirdBefore,
           "cleanup: popping orders ahead should not invalidate a later order's iterator");
    expect(isIndexed(book, 7), "cleanup: orders on the other side should be untouched");
    expect(!isIndexed(book, 2), "cleanup: an executing incoming order should never be indexed");
    expectIndexConsistent(book, "cleanup after partial fill");

    book.removeOrder(5);
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

}  // namespace

int main() {
    testExactFill();
    testPartialIncomingFill();
    testPartialRestingFill();
    testTradePrice();
    testTradeQuantity();
    testMakerAndTakerIds();
    testAggregateQuantityUpdates();
    testActiveIndexCleanup();
    testExecutionPreconditions();

    std::cout << "execution tests passed" << std::endl;
    return EXIT_SUCCESS;
}
