#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "order_book/book_event.hpp"
#include "order_book/fill_status.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"

namespace order_book {

// Befriended by OrderBook, PriceLevel, and Order so these tests can inspect internals, move
// the counters near their limits, and build malformed orders the Order constructor refuses.
struct OrderBookTestAccess {
    static const auto& index(const OrderBook& book) { return book.orderIndex; }
    static const auto& bids(const OrderBook& book) { return book.bids; }
    static const auto& asks(const OrderBook& book) { return book.asks; }
    static TradeId nextTradeId(const OrderBook& book) { return book.nextTradeId; }
    static void setNextTradeId(OrderBook& book, TradeId next) { book.nextTradeId = next; }
    static SequenceNum nextSequenceNum(const OrderBook& book) { return book.nextSequenceNum; }
    static void setNextSequenceNum(OrderBook& book, SequenceNum next) { book.nextSequenceNum = next; }

    static const std::list<Order>& orders(const PriceLevel& level) { return level.orders; }

    static void setPrice(Order& order, Price price) { order.priceTick = price; }
    static void setQuantity(Order& order, Quantity quantity) { order.initialQuant = order.remainingQuant = quantity; }
};

}  // namespace order_book

namespace {

using Access = order_book::OrderBookTestAccess;
using order_book::BookEvent;
using order_book::CancelRejected;
using order_book::FillStatus;
using order_book::Order;
using order_book::OrderAccepted;
using order_book::OrderBook;
using order_book::OrderCancelled;
using order_book::OrderId;
using order_book::OrderRejected;
using order_book::Price;
using order_book::Quantity;
using order_book::RejectionReason;
using order_book::SequenceNum;
using order_book::Side;
using order_book::TradeExecuted;
using order_book::TradeId;

using Events = std::vector<BookEvent>;

constexpr unsigned int maxValue = std::numeric_limits<unsigned int>::max();

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

std::size_t levelCount(const OrderBook& book, Side side) {
    return side == Side::BUY ? Access::bids(book).size() : Access::asks(book).size();
}

const order_book::PriceLevel& levelAt(const OrderBook& book, Side side, Price price) {
    return side == Side::BUY ? Access::bids(book).at(price) : Access::asks(book).at(price);
}

bool isIndexed(const OrderBook& book, OrderId orderId) {
    return Access::index(book).find(orderId) != Access::index(book).end();
}

// The IDs resting at one level, front (oldest) first.
std::vector<OrderId> queueAt(const OrderBook& book, Side side, Price price) {
    std::vector<OrderId> ids;
    for (const Order& order : Access::orders(levelAt(book, side, price))) {
        ids.push_back(order.getOrderId());
    }
    return ids;
}

// One index entry per resting order, pointing at it, no empty levels, aggregates matching
// their orders, and the active-order count equal to the number of resting orders; then the
// library's own checker, which also covers level ordering, FIFO, and an uncrossed book.
template <typename Levels>
void expectSideConsistent(const OrderBook& book, const Levels& levels, Side side, std::size_t& restingOrders,
                          const std::string& context) {
    for (const auto& [price, level] : levels) {
        expect(!level.isEmpty(), context + ": no empty price level should remain in the book");

        std::uint64_t remainingSum = 0;
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
    const order_book::InvariantResult invariants = book.checkInvariants();
    expect(invariants.isValid(),
           context + ": the book should pass its invariant check, got " + order_book::formatInvariantResult(invariants));
}

// Everything a command could disturb. Addresses show that a surviving order was not moved
// or reinserted; they are left null when comparing two different books, whose orders
// necessarily live at different addresses.
struct OrderState {
    OrderId id;
    SequenceNum sequenceNum;
    Quantity initialQuantity;
    Quantity remainingQuantity;
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
std::vector<LevelState> captureLevels(const Levels& levels, bool withAddresses) {
    std::vector<LevelState> captured;
    for (const auto& [price, level] : levels) {
        LevelState levelState{price, level.getAggregateQuantity(), {}};
        for (const Order& order : Access::orders(level)) {
            levelState.orders.push_back({order.getOrderId(), order.getSequenceNum(), order.getInitialQuantity(),
                                         order.getRemainingQuantity(), order.getExecutionLevel(),
                                         withAddresses ? &order : nullptr});
        }
        captured.push_back(levelState);
    }
    return captured;
}

BookState capture(const OrderBook& book, bool withAddresses = true) {
    std::vector<std::pair<OrderId, const Order*>> index;
    for (const auto& [orderId, location] : Access::index(book)) {
        index.emplace_back(orderId, withAddresses ? &*location.itr : nullptr);
    }
    std::sort(index.begin(), index.end());
    return {captureLevels(Access::bids(book), withAddresses), captureLevels(Access::asks(book), withAddresses),
            index, book.activeOrderCount(), Access::nextSequenceNum(book), Access::nextTradeId(book)};
}

// The state a successful cancellation of orderId should leave: that order gone from its
// level and the index, its quantity off the aggregate, its level gone if it was the last
// one there, and everything else, addresses included, exactly as it was.
BookState withoutOrder(BookState state, OrderId orderId) {
    for (std::vector<LevelState>* levels : {&state.bids, &state.asks}) {
        for (auto levelItr = levels->begin(); levelItr != levels->end(); ++levelItr) {
            auto& orders = levelItr->orders;
            const auto orderItr = std::find_if(orders.begin(), orders.end(),
                                               [orderId](const OrderState& order) { return order.id == orderId; });
            if (orderItr == orders.end()) {
                continue;
            }
            levelItr->aggregateQuantity -= orderItr->remainingQuantity;
            orders.erase(orderItr);
            if (orders.empty()) {
                levels->erase(levelItr);
            }
            std::erase_if(state.index, [orderId](const auto& entry) { return entry.first == orderId; });
            --state.activeOrders;
            return state;
        }
    }
    expect(false, "withoutOrder: order " + std::to_string(orderId) + " was not in the captured state");
    return state;
}

struct Submit {
    Order order;
};

// Rests without matching; the book throws rather than return a rejection.
struct Add {
    Order order;
};

struct Cancel {
    OrderId orderId;
};

using Command = std::variant<Submit, Add, Cancel>;

// What one command reported: its events, or, for an add the book refused, the message it threw.
struct Outcome {
    Events events;
    std::string refusal;

    bool operator==(const Outcome&) const = default;
};

Outcome run(OrderBook& book, const Command& command) {
    Outcome outcome;
    if (const Submit* submit = std::get_if<Submit>(&command)) {
        outcome.events = book.submitOrder(submit->order).getEvents().toVector();
    } else if (const Add* add = std::get_if<Add>(&command)) {
        try {
            outcome.events = book.addOrder(add->order).getEvents().toVector();
        } catch (const std::invalid_argument& error) {
            outcome.refusal = error.what();
        }
    } else {
        outcome.events = book.cancelOrder(std::get<Cancel>(command).orderId).getEvents().toVector();
    }
    return outcome;
}

// Runs every command on a fresh book, checking it after each one, and collects the outcomes.
std::vector<Outcome> replay(const std::vector<Command>& commands, BookState& finalState, const std::string& context) {
    OrderBook book;
    std::vector<Outcome> outcomes;
    for (std::size_t i = 0; i < commands.size(); ++i) {
        outcomes.push_back(run(book, commands[i]));
        expectBookConsistent(book, context + " after command " + std::to_string(i));
    }
    finalState = capture(book, false);
    return outcomes;
}

// Both sides fill outward from the spread one level at a time, then alternating takers walk
// back in, each taking exactly one level, until the book is empty.
void testAlternatingBuysAndSells() {
    const std::string context = "alternating buys and sells";
    constexpr unsigned int levels = 10;
    OrderBook book;

    for (unsigned int i = 0; i < levels; ++i) {
        const OrderId bidId = 2 * i + 1;
        const OrderId askId = 2 * i + 2;
        expect(book.submitOrder(Order(bidId, 100 - i, 10, 1, Side::BUY)).getEvents() ==
                   Events{OrderAccepted{bidId, Side::BUY, 100 - i, 10}},
               context + ": a passive buy should only be accepted");
        expect(book.submitOrder(Order(askId, 101 + i, 10, 1, Side::SELL)).getEvents() ==
                   Events{OrderAccepted{askId, Side::SELL, 101 + i, 10}},
               context + ": a passive sell should only be accepted");
        expectBookConsistent(book, context + " building level " + std::to_string(i));
    }
    expect(levelCount(book, Side::BUY) == levels && levelCount(book, Side::SELL) == levels,
           context + ": each side should hold one level per resting order");
    expect(book.getBestBid().getOrderId() == 1 && book.getBestAsk().getOrderId() == 2,
           context + ": the first two orders should still be the best on each side");

    for (unsigned int k = 0; k < levels; ++k) {
        const std::string step = context + " taking level " + std::to_string(k);
        const OrderId buyer = 101 + 2 * k;
        const OrderId seller = 102 + 2 * k;

        expect(book.submitOrder(Order(buyer, 101 + k, 10, 1, Side::BUY)).getEvents() ==
                   Events{OrderAccepted{buyer, Side::BUY, 101 + k, 10}, TradeExecuted{buyer, 2 * k + 2, 101 + k, 10}},
               step + ": the buy should take exactly the best ask");
        expectBookConsistent(book, step + " (buy)");
        expect(k + 1 == levels ? !book.hasAsks() : book.getBestAsk().getPriceTick() == 102 + k,
               step + ": the best ask should move out one tick");

        expect(book.submitOrder(Order(seller, 100 - k, 10, 1, Side::SELL)).getEvents() ==
                   Events{OrderAccepted{seller, Side::SELL, 100 - k, 10}, TradeExecuted{seller, 2 * k + 1, 100 - k, 10}},
               step + ": the sell should take exactly the best bid");
        expectBookConsistent(book, step + " (sell)");
        expect(k + 1 == levels ? !book.hasBids() : book.getBestBid().getPriceTick() == 99 - k,
               step + ": the best bid should move out one tick");
    }
    expect(book.activeOrderCount() == 0, context + ": every order should have traded");
    expect(Access::nextTradeId(book) == 2 * levels + 1, context + ": one trade ID per taker");
    expect(Access::nextSequenceNum(book) == 4 * levels + 1, context + ": one sequence number per order");
}

// At one price, each order is larger than what the previous one left, so it takes that
// remainder and rests its own: the book holds one order at a time, on alternating sides.
void testAlternatingSidesAtOnePrice() {
    const std::string context = "alternating sides at one price";
    OrderBook book;
    book.submitOrder(Order(1, 100, 10, 1, Side::BUY));
    Quantity resting = 10;

    for (OrderId id = 2; id <= 30; ++id) {
        const std::string step = context + " order " + std::to_string(id);
        const Side side = id % 2 == 0 ? Side::SELL : Side::BUY;
        const Quantity quantity = 5 * (id + 1);

        expect(book.submitOrder(Order(id, 100, quantity, 1, side)).getEvents() ==
                   Events{OrderAccepted{id, side, 100, quantity}, TradeExecuted{id, id - 1, 100, resting}},
               step + ": should take the whole of the previous order's remainder");
        resting = quantity - resting;

        expectBookConsistent(book, step);
        expect(book.activeOrderCount() == 1 && !hasSide(book, opposite(side)),
               step + ": only the new order's remainder should rest");
        const Order rested = bestOn(book, side);
        expect(rested.getOrderId() == id && rested.getRemainingQuantity() == resting &&
                   rested.getExecutionLevel() == FillStatus::PARTIAL,
               step + ": its partially filled remainder should rest at 100");
    }
}

// Hundreds of orders queue at one price in acceptance order, and takers, cancellations,
// and a final sweep all respect that queue.
void testManyOrdersAtOnePrice(Side restingSide) {
    const std::string context = "many orders at one price (resting " + sideName(restingSide) + ")";
    const Side incomingSide = opposite(restingSide);
    constexpr OrderId count = 250;
    auto quantityOf = [](OrderId id) { return id % 7 + 1; };
    OrderBook book;

    Quantity total = 0;
    for (OrderId id = 1; id <= count; ++id) {
        book.submitOrder(Order(id, 100, quantityOf(id), 1, restingSide));
        total += quantityOf(id);
    }
    expectBookConsistent(book, context + " after queueing");
    expect(levelCount(book, restingSide) == 1, context + ": every order should share one level");
    const order_book::PriceLevel& level = levelAt(book, restingSide, 100);
    expect(level.getOrdersQuantity() == count && level.getAggregateQuantity() == total,
           context + ": the level should hold every order and their whole quantity");
    SequenceNum expectedSequence = 1;
    for (const Order& order : Access::orders(level)) {
        expect(order.getOrderId() == expectedSequence && order.getSequenceNum() == expectedSequence,
               context + ": the queue should be in acceptance order");
        ++expectedSequence;
    }
    const order_book::BookSnapshot snapshot = book.snapshot(5);
    const auto& summaries = restingSide == Side::BUY ? snapshot.bids : snapshot.asks;
    expect(summaries.size() == 1 && summaries[0].price == 100 && summaries[0].totalQuantity == total &&
               summaries[0].orderQuantity == count && snapshot.totalActiveVisibleOrders == count,
           context + ": the snapshot should show one level with every order");

    // Takes the first 100 orders whole and 3 of order 101's 4.
    Quantity firstHundred = 0;
    Events expected{OrderAccepted{1000, incomingSide, 100, 0}};
    for (OrderId id = 1; id <= 100; ++id) {
        expected.push_back(TradeExecuted{1000, id, 100, quantityOf(id)});
        firstHundred += quantityOf(id);
    }
    expected.push_back(TradeExecuted{1000, 101, 100, 3});
    std::get<OrderAccepted>(expected[0]).quantity = firstHundred + 3;
    expect(book.submitOrder(Order(1000, 100, firstHundred + 3, 1, incomingSide)).getEvents() == expected,
           context + ": the taker should fill the queue oldest first");
    expectBookConsistent(book, context + " after the first taker");
    const Order front = bestOn(book, restingSide);
    expect(front.getOrderId() == 101 && front.getSequenceNum() == 101 && front.getRemainingQuantity() == 1 &&
               front.getExecutionLevel() == FillStatus::PARTIAL,
           context + ": the partly filled order should keep its place at the front");

    // Cancelling every even ID leaves the odd ones queued as they were.
    for (OrderId id = 102; id <= count; id += 2) {
        expect(book.cancelOrder(id).getEvents() == Events{OrderCancelled{id, quantityOf(id)}},
               context + ": each cancellation should report the order's whole quantity");
    }
    expectBookConsistent(book, context + " after cancelling the even IDs");
    std::vector<OrderId> odd;
    for (OrderId id = 101; id <= count; id += 2) {
        odd.push_back(id);
    }
    expect(queueAt(book, restingSide, 100) == odd, context + ": the surviving orders should keep their order");

    // A sweep larger than the level takes every survivor in turn and rests the rest.
    Events sweep{OrderAccepted{1001, incomingSide, 100, 10000}, TradeExecuted{1001, 101, 100, 1}};
    Quantity swept = 1;
    for (OrderId id = 103; id <= count; id += 2) {
        sweep.push_back(TradeExecuted{1001, id, 100, quantityOf(id)});
        swept += quantityOf(id);
    }
    expect(book.submitOrder(Order(1001, 100, 10000, 1, incomingSide)).getEvents() == sweep,
           context + ": the sweep should fill the survivors in queue order");
    expectBookConsistent(book, context + " after the sweep");
    expect(!hasSide(book, restingSide), context + ": the sweep should empty the resting side");
    expect(book.activeOrderCount() == 1 && bestOn(book, incomingSide).getRemainingQuantity() == 10000 - swept,
           context + ": only the sweep's remainder should rest");
}

// Hundreds of levels per side, inserted out of price order, are kept best first; a taker
// sweeps exactly as many levels as its limit allows, and the other side is swept whole.
void testManyPriceLevels() {
    const std::string context = "many price levels";
    constexpr unsigned int levels = 400;
    OrderBook book;

    // 157 is coprime with 400, so this visits every offset 1..400 once, out of order.
    for (unsigned int j = 0; j < levels; ++j) {
        const unsigned int offset = j * 157 % levels + 1;
        book.submitOrder(Order(offset, 1000 - offset, offset, 1, Side::BUY));
        book.submitOrder(Order(levels + offset, 1000 + offset, offset, 1, Side::SELL));
    }
    expectBookConsistent(book, context + " after building");
    expect(levelCount(book, Side::BUY) == levels && levelCount(book, Side::SELL) == levels,
           context + ": each order should open its own level");

    const order_book::BookSnapshot full = book.snapshot(levels);
    expect(full.bids.size() == levels && full.asks.size() == levels && full.totalActiveVisibleOrders == 2 * levels,
           context + ": a full-depth snapshot should show every level");
    for (unsigned int offset = 1; offset <= levels; ++offset) {
        const order_book::PriceLevelSummary& bid = full.bids[offset - 1];
        const order_book::PriceLevelSummary& ask = full.asks[offset - 1];
        expect(bid.price == 1000 - offset && bid.totalQuantity == offset && bid.orderQuantity == 1,
               context + ": bids should be listed best (highest) first");
        expect(ask.price == 1000 + offset && ask.totalQuantity == offset && ask.orderQuantity == 1,
               context + ": asks should be listed best (lowest) first");
    }
    const order_book::BookSnapshot top = book.snapshot(10);
    expect(top.bids.size() == 10 && top.asks.size() == 10 && top.bids.back().price == 990 &&
               top.asks.back().price == 1010,
           context + ": a shallow snapshot should show only the best levels");

    // A buy limited to 1200 takes the 200 asks up to it, in price order, and rests 7 there.
    Events buy{OrderAccepted{5000, Side::BUY, 1200, 0}};
    Quantity cost = 0;
    for (unsigned int offset = 1; offset <= 200; ++offset) {
        buy.push_back(TradeExecuted{5000, levels + offset, 1000 + offset, offset});
        cost += offset;
    }
    std::get<OrderAccepted>(buy[0]).quantity = cost + 7;
    expect(book.submitOrder(Order(5000, 1200, cost + 7, 1, Side::BUY)).getEvents() == buy,
           context + ": the buy should take every ask up to its limit, cheapest first");
    expectBookConsistent(book, context + " after the buy sweep");
    expect(levelCount(book, Side::SELL) == levels - 200 && book.getBestAsk().getPriceTick() == 1201,
           context + ": the asks beyond the limit should be untouched");
    expect(book.getBestBid().getOrderId() == 5000 && book.getBestBid().getRemainingQuantity() == 7,
           context + ": the remainder should be the new best bid");

    // A sell at the lowest price takes that remainder, then every bid, highest first.
    Events sell{OrderAccepted{5001, Side::SELL, 1, 1000000}, TradeExecuted{5001, 5000, 1200, 7}};
    Quantity sold = 7;
    for (unsigned int offset = 1; offset <= levels; ++offset) {
        sell.push_back(TradeExecuted{5001, offset, 1000 - offset, offset});
        sold += offset;
    }
    expect(book.submitOrder(Order(5001, 1, 1000000, 1, Side::SELL)).getEvents() == sell,
           context + ": the sell should take every bid, highest first");
    expectBookConsistent(book, context + " after the sell sweep");
    expect(!book.hasBids(), context + ": every bid should be gone");
    expect(book.getBestAsk().getOrderId() == 5001 && book.getBestAsk().getRemainingQuantity() == 1000000 - sold,
           context + ": the remainder should be the new best ask, at 1");
    expect(levelCount(book, Side::SELL) == levels - 200 + 1, context + ": the remainder should open its own level");
}

// One large resting order is filled a little at a time: it keeps its place, sequence number,
// and initial quantity throughout, and the order behind it waits until it is gone. Then a
// remainder that rested from a submission is filled a little at a time as a maker.
void testRepeatedPartialFills(Side restingSide) {
    const std::string context = "repeated partial fills (resting " + sideName(restingSide) + ")";
    const Side incomingSide = opposite(restingSide);
    OrderBook book;
    book.submitOrder(Order(1, 100, 1000, 1, restingSide));
    book.submitOrder(Order(2, 100, 5, 1, restingSide));

    for (OrderId k = 1; k <= 142; ++k) {
        const std::string step = context + " fill " + std::to_string(k);
        const OrderId taker = 100 + k;
        expect(book.submitOrder(Order(taker, 100, 7, 1, incomingSide)).getEvents() ==
                   Events{OrderAccepted{taker, incomingSide, 100, 7}, TradeExecuted{taker, 1, 100, 7}},
               step + ": each taker should trade only with the front order");
        expectBookConsistent(book, step);
        const Order front = bestOn(book, restingSide);
        expect(front.getOrderId() == 1 && front.getSequenceNum() == 1 && front.getInitialQuantity() == 1000 &&
                   front.getRemainingQuantity() == 1000 - 7 * k && front.getExecutionLevel() == FillStatus::PARTIAL,
               step + ": the front order should keep its place and record each fill");
        expect(levelAt(book, restingSide, 100).getAggregateQuantity() == 1000 - 7 * k + 5,
               step + ": the level aggregate should fall by each fill");
        expect(!isIndexed(book, taker) && Access::nextTradeId(book) == k + 1,
               step + ": each filled taker should leave nothing resting and use one trade ID");
    }

    // 6 left on order 1; this taker finishes it and starts on order 2.
    expect(book.submitOrder(Order(300, 100, 8, 1, incomingSide)).getEvents() ==
               Events{OrderAccepted{300, incomingSide, 100, 8}, TradeExecuted{300, 1, 100, 6},
                      TradeExecuted{300, 2, 100, 2}},
           context + ": the last fill should finish the front order and move to the next");
    expectBookConsistent(book, context + " after finishing the front order");
    expect(!isIndexed(book, 1) && bestOn(book, restingSide).getOrderId() == 2 &&
               bestOn(book, restingSide).getRemainingQuantity() == 3,
           context + ": the order behind should now be at the front");

    // Takes the last 3 of order 2 and rests 47 on the incoming side; 501 queues behind it.
    book.submitOrder(Order(500, 100, 50, 1, incomingSide));
    book.submitOrder(Order(501, 100, 10, 1, incomingSide));
    expect(!hasSide(book, restingSide), context + ": the resting side should be empty");
    for (OrderId k = 1; k <= 11; ++k) {
        const std::string step = context + " maker fill " + std::to_string(k);
        const OrderId taker = 600 + k;
        expect(book.submitOrder(Order(taker, 100, 4, 1, restingSide)).getEvents() ==
                   Events{OrderAccepted{taker, restingSide, 100, 4}, TradeExecuted{taker, 500, 100, 4}},
               step + ": each taker should trade only with the rested remainder");
        expectBookConsistent(book, step);
        const Order remainder = book.findActiveOrder(500);
        expect(remainder.getRemainingQuantity() == 47 - 4 * k && remainder.getInitialQuantity() == 50 &&
                   remainder.getExecutionLevel() == FillStatus::PARTIAL && bestOn(book, incomingSide).getOrderId() == 500,
               step + ": the remainder should stay at the front, recording each fill");
    }
    expect(book.submitOrder(Order(700, 100, 5, 1, restingSide)).getEvents() ==
               Events{OrderAccepted{700, restingSide, 100, 5}, TradeExecuted{700, 500, 100, 3},
                      TradeExecuted{700, 501, 100, 2}},
           context + ": the last fill should finish the remainder and move to the order behind it");
    expectBookConsistent(book, context + " at the end");
    expect(book.findActiveOrder(501).getRemainingQuantity() == 8 && book.activeOrderCount() == 1,
           context + ": only the second order should be left, partly filled");
}

// Every resting order is cancelled, in a scrambled order, and each cancellation is repeated.
// Each one removes exactly that order; a repeat is refused and changes nothing.
void testRepeatedCancellations() {
    const std::string context = "repeated cancellations";
    constexpr OrderId count = 60;
    OrderBook book;
    // Bids 1..30 across 95..99 and asks 31..60 across 101..105, six to a level.
    for (OrderId id = 1; id <= count; ++id) {
        const unsigned int level = (id - 1) % 30 / 6;
        if (id <= 30) {
            book.submitOrder(Order(id, 95 + level, id, 1, Side::BUY));
        } else {
            book.submitOrder(Order(id, 101 + level, id, 1, Side::SELL));
        }
    }
    expectBookConsistent(book, context + " after building");
    const SequenceNum nextSequence = Access::nextSequenceNum(book);

    // 17 is coprime with 60, so this visits every ID once, out of order.
    for (unsigned int j = 0; j < count; ++j) {
        const OrderId id = j * 17 % count + 1;
        const std::string step = context + " of " + std::to_string(id);
        const BookState expected = withoutOrder(capture(book), id);

        expect(book.cancelOrder(id).getEvents() == Events{OrderCancelled{id, id}},
               step + ": should report the order's whole quantity");
        expect(capture(book) == expected, step + ": should remove only that order, leaving the rest in place");
        expectBookConsistent(book, step);

        for (int repeat = 0; repeat < 3; ++repeat) {
            expect(book.cancelOrder(id).getEvents() == Events{CancelRejected{id, RejectionReason::ORDER_NOT_FOUND}},
                   step + ": a repeat should be refused");
            expect(capture(book) == expected, step + ": a repeat should change nothing");
        }
    }
    expect(book.activeOrderCount() == 0 && !book.hasBids() && !book.hasAsks(),
           context + ": the book should be empty once every order is cancelled");
    expect(Access::nextSequenceNum(book) == nextSequence && Access::nextTradeId(book) == 1,
           context + ": cancellations should consume no sequence number or trade ID");
    expectBookConsistent(book, context + " at the end");
}

// A cancelled ID is free again, but a resubmission is a new order: it joins the back of its
// level's queue, can recreate a level its cancellation emptied, and can trade on either side.
void testSubmitAfterCancellation() {
    const std::string context = "submit after cancellation";
    OrderBook book;
    book.submitOrder(Order(1, 100, 5, 1, Side::SELL));
    book.submitOrder(Order(2, 100, 5, 1, Side::SELL));
    book.submitOrder(Order(3, 100, 5, 1, Side::SELL));
    book.submitOrder(Order(4, 101, 5, 1, Side::SELL));

    book.cancelOrder(1);
    expect(book.submitOrder(Order(1, 100, 5, 1, Side::SELL)).getEvents() ==
               Events{OrderAccepted{1, Side::SELL, 100, 5}},
           context + ": the cancelled ID should be accepted again");
    expect(queueAt(book, Side::SELL, 100) == std::vector<OrderId>{2, 3, 1},
           context + ": the resubmitted order should lose its time priority");
    expect(book.findActiveOrder(1).getSequenceNum() == 5, context + ": it should be numbered as a new acceptance");
    expectBookConsistent(book, context + " after resubmitting at the same price");

    book.cancelOrder(4);
    expect(levelCount(book, Side::SELL) == 1, context + ": cancelling the only order at 101 should erase its level");
    book.submitOrder(Order(4, 101, 2, 1, Side::SELL));
    expect(queueAt(book, Side::SELL, 101) == std::vector<OrderId>{4},
           context + ": resubmitting at 101 should recreate the level");
    expectBookConsistent(book, context + " after recreating a level");

    // ID 2, cancelled as a sell, comes back as a buy and trades through 3 and part of 1.
    book.cancelOrder(2);
    expect(book.submitOrder(Order(2, 100, 7, 1, Side::BUY)).getEvents() ==
               Events{OrderAccepted{2, Side::BUY, 100, 7}, TradeExecuted{2, 3, 100, 5}, TradeExecuted{2, 1, 100, 2}},
           context + ": a resubmission on the other side should trade like any other order");
    expectBookConsistent(book, context + " after resubmitting on the other side");

    expect(book.cancelOrder(1).getEvents() == Events{OrderCancelled{1, 3}},
           context + ": cancelling the partly filled resubmission should report what it had left");
    expect(book.submitOrder(Order(1, 99, 4, 1, Side::BUY)).getEvents() == Events{OrderAccepted{1, Side::BUY, 99, 4}},
           context + ": the ID should be free once more");
    book.cancelOrder(4);
    expect(book.submitOrder(Order(4, 99, 4, 1, Side::SELL)).getEvents() ==
               Events{OrderAccepted{4, Side::SELL, 99, 4}, TradeExecuted{4, 1, 99, 4}},
           context + ": the last resubmission should fill against the resubmitted bid exactly");
    expectBookConsistent(book, context + " at the end");
    expect(book.activeOrderCount() == 0, context + ": nothing should be left resting");
    expect(Access::nextSequenceNum(book) == 10 && Access::nextTradeId(book) == 4,
           context + ": nine acceptances and three trades");
}

// The price n levels out from the spread on a side: bids below 100, asks above it.
Price outward(Side side, unsigned int n) {
    return side == Side::BUY ? 99 - n : 101 + n;
}

// One side is emptied by a sweep and later by cancellation. While it is empty, the other
// side's orders rest at any price, even the most aggressive one, without trading.
void testEmptyingOneSide(Side emptiedSide) {
    const std::string context = "emptying the " + sideName(emptiedSide) + " side";
    const Side otherSide = opposite(emptiedSide);
    OrderBook book;
    book.submitOrder(Order(1, outward(emptiedSide, 0), 4, 1, emptiedSide));
    book.submitOrder(Order(2, outward(emptiedSide, 0), 5, 1, emptiedSide));
    book.submitOrder(Order(3, outward(emptiedSide, 1), 6, 1, emptiedSide));
    book.submitOrder(Order(11, outward(otherSide, 0), 7, 1, otherSide));
    book.submitOrder(Order(12, outward(otherSide, 2), 8, 1, otherSide));

    const BookState before = capture(book);
    const auto& otherBefore = otherSide == Side::BUY ? before.bids : before.asks;
    expect(book.submitOrder(Order(20, outward(emptiedSide, 1), 15, 1, otherSide)).getEvents() ==
               Events{OrderAccepted{20, otherSide, outward(emptiedSide, 1), 15},
                      TradeExecuted{20, 1, outward(emptiedSide, 0), 4}, TradeExecuted{20, 2, outward(emptiedSide, 0), 5},
                      TradeExecuted{20, 3, outward(emptiedSide, 1), 6}},
           context + ": an exact sweep should take every order on the side");
    expectBookConsistent(book, context + " after the sweep");
    expect(!hasSide(book, emptiedSide), context + ": the swept side should be empty");
    expectThrows([&book, emptiedSide] { bestOn(book, emptiedSide); },
                 context + ": asking for the best of an empty side should throw");
    const order_book::BookSnapshot snapshot = book.snapshot();
    expect((emptiedSide == Side::BUY ? snapshot.bids : snapshot.asks).empty() && snapshot.totalActiveVisibleOrders == 2,
           context + ": the snapshot should show nothing on the swept side");
    const BookState after = capture(book);
    expect((otherSide == Side::BUY ? after.bids : after.asks) == otherBefore,
           context + ": the other side should be untouched by the sweep");

    // Nothing to cross, so the most aggressive price possible still rests.
    const Price extreme = otherSide == Side::BUY ? maxValue : 1;
    expect(book.submitOrder(Order(21, extreme, 3, 1, otherSide)).getEvents() ==
               Events{OrderAccepted{21, otherSide, extreme, 3}},
           context + ": an order facing an empty side should rest without trading");
    expect(bestOn(book, otherSide).getOrderId() == 21, context + ": it should be the new best on its side");
    expect(book.addOrder(Order(22, extreme, 3, 1, otherSide)).getEvents() ==
               Events{OrderAccepted{22, otherSide, extreme, 3}},
           context + ": an add facing an empty side cannot cross, so it should be accepted");
    expectBookConsistent(book, context + " with aggressive orders resting");
    book.cancelOrder(21);
    book.cancelOrder(22);

    // Refilled, then emptied again by cancelling, and then the other side too.
    book.submitOrder(Order(1, outward(emptiedSide, 0), 3, 1, emptiedSide));
    expect(hasSide(book, emptiedSide), context + ": the side should accept new orders once empty");
    expect(book.cancelOrder(1).getEvents() == Events{OrderCancelled{1, 3}},
           context + ": cancelling the refill should report its quantity");
    expect(!hasSide(book, emptiedSide), context + ": cancelling its only order should empty the side again");
    expectBookConsistent(book, context + " after emptying by cancellation");
    book.cancelOrder(11);
    book.cancelOrder(12);
    expect(!book.hasBids() && !book.hasAsks() && book.activeOrderCount() == 0,
           context + ": cancelling the other side should leave the book empty");
    expectBookConsistent(book, context + " at the end");
}

// A command the book should refuse, and the reason it should give; a valid command has none.
struct Step {
    Command command;
    std::optional<RejectionReason> refusal;
};

Order zeroPriced(OrderId id) {
    Order order(id, 100, 5, 1, Side::BUY);
    Access::setPrice(order, 0);
    return order;
}

Order zeroQuantity(OrderId id) {
    Order order(id, 100, 5, 1, Side::SELL);
    Access::setQuantity(order, 0);
    return order;
}

Order markedFilled(OrderId id) {
    Order order(id, 100, 5, 1, Side::SELL);
    order.setExecutionLevel(FillStatus::FILLED);
    return order;
}

// Every refused command leaves the book exactly as it was, with no sequence number or trade
// ID consumed, so the valid commands report the same outcomes and leave the same book as
// they do with the refused ones taken out.
void testInvalidCommandsMixedWithValid() {
    const std::string context = "invalid commands mixed with valid";
    const std::vector<Step> script{
        {Submit{Order(1, 101, 5, 1, Side::SELL)}, std::nullopt},
        {Submit{Order(2, 99, 4, 1, Side::BUY)}, std::nullopt},
        {Submit{Order(1, 100, 1, 1, Side::BUY)}, RejectionReason::DUPLICATE_ORDER_ID},
        {Submit{zeroPriced(3)}, RejectionReason::INVALID_PRICE},
        {Add{Order(3, 102, 6, 1, Side::SELL)}, std::nullopt},
        {Add{Order(4, 101, 2, 1, Side::BUY)}, RejectionReason::WOULD_CROSS_BOOK},
        {Cancel{99}, RejectionReason::ORDER_NOT_FOUND},
        {Submit{Order(4, 101, 2, 1, Side::BUY)}, std::nullopt},  // takes 2 of 1
        {Cancel{4}, RejectionReason::ORDER_NOT_FOUND},           // filled without resting
        {Submit{markedFilled(5)}, RejectionReason::ORDER_NOT_ACTIVE},
        {Add{Order(2, 105, 1, 1, Side::SELL)}, RejectionReason::DUPLICATE_ORDER_ID},
        {Cancel{2}, std::nullopt},
        {Cancel{2}, RejectionReason::ORDER_NOT_FOUND},
        {Cancel{0}, RejectionReason::ORDER_NOT_FOUND},
        {Submit{Order(2, 102, 10, 1, Side::BUY)}, std::nullopt},  // takes 3 of 1 and 6 of 3, rests 1
        {Submit{Order(2, 90, 1, 1, Side::SELL)}, RejectionReason::DUPLICATE_ORDER_ID},
        {Add{zeroQuantity(6)}, RejectionReason::INVALID_QUANTITY},
        {Submit{Order(6, 102, 1, 1, Side::SELL)}, std::nullopt},  // takes the last 1 of 2
        {Cancel{6}, RejectionReason::ORDER_NOT_FOUND},
    };

    OrderBook mixed;
    std::vector<Outcome> validOutcomes;
    for (std::size_t i = 0; i < script.size(); ++i) {
        const Step& step = script[i];
        const std::string stepContext = context + " command " + std::to_string(i);
        const BookState before = capture(mixed);
        const Outcome outcome = run(mixed, step.command);
        expectBookConsistent(mixed, stepContext);
        if (!step.refusal) {
            expect(outcome.refusal.empty(), stepContext + ": a valid add should not be refused");
            validOutcomes.push_back(outcome);
            continue;
        }

        Outcome refused;
        if (const Submit* submit = std::get_if<Submit>(&step.command)) {
            refused.events = {OrderRejected{submit->order.getOrderId(), *step.refusal}};
        } else if (std::holds_alternative<Add>(step.command)) {
            refused.refusal = order_book::rejectionMessage(*step.refusal);
        } else {
            refused.events = {CancelRejected{std::get<Cancel>(step.command).orderId, *step.refusal}};
        }
        expect(outcome == refused, stepContext + ": should be refused with " +
                                       order_book::rejectionReasonName(*step.refusal) + " and nothing else");
        expect(capture(mixed) == before, stepContext + ": a refused command should leave the book exactly as it was");
    }

    std::vector<Command> validOnly;
    for (const Step& step : script) {
        if (!step.refusal) {
            validOnly.push_back(step.command);
        }
    }
    BookState validState;
    expect(replay(validOnly, validState, context + " (valid only)") == validOutcomes,
           context + ": the valid commands should report the same outcomes without the refused ones");
    expect(capture(mixed, false) == validState,
           context + ": the refused commands should leave no trace in the final book or its counters");

    expect(mixed.activeOrderCount() == 0, context + ": every valid order should have traded or been cancelled");
    expect(Access::nextSequenceNum(mixed) == 7 && Access::nextTradeId(mixed) == 5,
           context + ": six acceptances and four trades, none for the refused commands");
}

// Prices, IDs, and quantities at their maximum, a level aggregate at and beyond the maximum
// Quantity, and the sequence and trade counters wrapping in the middle of a scenario.
void testCommandsNearNumericLimits() {
    const std::string context = "near numeric limits";
    {
        // The highest bid meets the lowest ask, each for the largest quantity, under the
        // largest IDs; the trade is at the resting (maximum) price.
        OrderBook book;
        expect(book.submitOrder(Order(maxValue, maxValue, maxValue, 1, Side::BUY)).getEvents() ==
                   Events{OrderAccepted{maxValue, Side::BUY, maxValue, maxValue}},
               context + ": a maximal bid should rest");
        expect(book.submitOrder(Order(maxValue - 1, 1, maxValue, 1, Side::SELL)).getEvents() ==
                   Events{OrderAccepted{maxValue - 1, Side::SELL, 1, maxValue},
                          TradeExecuted{maxValue - 1, maxValue, maxValue, maxValue}},
               context + ": a minimal-price sell should take the maximal bid whole, at the maximum price");
        expect(book.activeOrderCount() == 0, context + ": both maximal orders should be filled");
        expectBookConsistent(book, context + " after maximal orders trade");

        // The lowest price takes the smallest quantity, and the rest of a maximal buy rests there.
        book.submitOrder(Order(1, 1, 1, 1, Side::SELL));
        expect(book.submitOrder(Order(maxValue, 1, maxValue, 1, Side::BUY)).getEvents() ==
                   Events{OrderAccepted{maxValue, Side::BUY, 1, maxValue}, TradeExecuted{maxValue, 1, 1, 1}},
               context + ": a maximal buy at the lowest price should take the one unit there");
        expectBookConsistent(book, context + " after a maximal buy at the lowest price");
        expect(book.cancelOrder(maxValue).getEvents() == Events{OrderCancelled{maxValue, maxValue - 1}},
               context + ": cancelling the maximal remainder should report all but one unit");
    }
    {
        // A level whose aggregate is exactly the maximum, then taken whole.
        OrderBook book;
        book.submitOrder(Order(10, 50, maxValue - 1, 1, Side::SELL));
        book.submitOrder(Order(11, 50, 1, 1, Side::SELL));
        expectBookConsistent(book, context + " with a level at the maximum aggregate");
        expect(levelAt(book, Side::SELL, 50).getAggregateQuantity() == maxValue &&
                   book.snapshot().asks[0].totalQuantity == maxValue,
               context + ": the level should hold the maximum quantity");
        expect(book.submitOrder(Order(12, 50, maxValue, 1, Side::BUY)).getEvents() ==
                   Events{OrderAccepted{12, Side::BUY, 50, maxValue}, TradeExecuted{12, 10, 50, maxValue - 1},
                          TradeExecuted{12, 11, 50, 1}},
               context + ": a maximal buy should take the whole level");
        expect(book.activeOrderCount() == 0, context + ": the level should be emptied");
        expectBookConsistent(book, context + " after taking the maximal level");
    }
    {
        // A level holding more than one Quantity can: the aggregate is wider, so it neither
        // wraps as orders join nor as they fill or are cancelled.
        constexpr std::uint64_t maxQuantity = maxValue;
        OrderBook book;
        for (OrderId id = 30; id <= 32; ++id) {
            book.submitOrder(Order(id, 50, maxValue, 1, Side::SELL));
            expectBookConsistent(book, context + " filling a level past the maximum");
        }
        expect(levelAt(book, Side::SELL, 50).getAggregateQuantity() == 3 * maxQuantity &&
                   book.snapshot().asks[0].totalQuantity == 3 * maxQuantity,
               context + ": three maximal orders should sum to three times the maximum, unwrapped");

        expect(book.cancelOrder(31).getEvents() == Events{OrderCancelled{31, maxValue}},
               context + ": cancelling a maximal order from the middle should report all of it");
        expect(levelAt(book, Side::SELL, 50).getAggregateQuantity() == 2 * maxQuantity,
               context + ": the cancellation should take one maximum off the aggregate");
        expectBookConsistent(book, context + " after cancelling past the maximum");

        expect(book.submitOrder(Order(33, 50, maxValue, 1, Side::BUY)).getEvents() ==
                   Events{OrderAccepted{33, Side::BUY, 50, maxValue}, TradeExecuted{33, 30, 50, maxValue}},
               context + ": a maximal buy should take the front order whole");
        expect(book.submitOrder(Order(34, 50, 1, 1, Side::BUY)).getEvents() ==
                   Events{OrderAccepted{34, Side::BUY, 50, 1}, TradeExecuted{34, 32, 50, 1}},
               context + ": a one-unit buy should take one unit from the last order");
        expect(levelAt(book, Side::SELL, 50).getAggregateQuantity() == maxQuantity - 1 &&
                   queueAt(book, Side::SELL, 50) == std::vector<OrderId>{32},
               context + ": the fills should bring the aggregate back below the maximum");
        expectBookConsistent(book, context + " after filling back below the maximum");
    }
    {
        // Sequence numbers wrap while orders queue at one level, and trade IDs wrap while
        // those orders fill; time priority follows queue position throughout.
        OrderBook book;
        Access::setNextSequenceNum(book, maxValue - 1);
        for (OrderId id = 20; id <= 23; ++id) {
            book.addOrder(Order(id, 60, 1, 1, Side::SELL));
            expectBookConsistent(book, context + " queueing across the sequence wrap");
        }
        std::vector<SequenceNum> sequenceNums;
        for (const Order& order : Access::orders(levelAt(book, Side::SELL, 60))) {
            sequenceNums.push_back(order.getSequenceNum());
        }
        expect(sequenceNums == std::vector<SequenceNum>{maxValue - 1, maxValue, 1, 2},
               context + ": sequence numbers should wrap past the maximum to 1, skipping zero");

        Access::setNextTradeId(book, maxValue);
        const order_book::SubmissionResults result = book.submitOrder(Order(24, 60, 4, 1, Side::BUY));
        expect(result.getEvents() == Events{OrderAccepted{24, Side::BUY, 60, 4}, TradeExecuted{24, 20, 60, 1},
                                            TradeExecuted{24, 21, 60, 1}, TradeExecuted{24, 22, 60, 1},
                                            TradeExecuted{24, 23, 60, 1}},
               context + ": the queue should fill in acceptance order across the wrap");
        std::vector<TradeId> tradeIds;
        for (const order_book::Trade& trade : result.getGeneratedTrades()) {
            tradeIds.push_back(trade.getTradeId());
        }
        expect(tradeIds == std::vector<TradeId>{maxValue, 1, 2, 3},
               context + ": trade IDs should wrap past the maximum to 1, skipping zero");
        expect(Access::nextSequenceNum(book) == 4 && Access::nextTradeId(book) == 4,
               context + ": both counters should continue from where they wrapped");
        expectBookConsistent(book, context + " after the wraps");
    }
}

// A fixed linear congruential generator, so the script is the same on every platform and
// standard library (std::uniform_int_distribution's output is implementation-defined).
class ScriptRandom {
public:
    explicit ScriptRandom(std::uint64_t seed) : state(seed) {}

    // A value in [0, bound).
    unsigned int below(unsigned int bound) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<unsigned int>((state >> 33) % bound);
    }

private:
    std::uint64_t state;
};

// Submits, adds, and cancels around a narrow spread, from a small pool of IDs, so the
// script trades often and also hits duplicates, reused IDs, crossing adds, unknown and
// repeated cancels, and malformed orders.
std::vector<Command> randomScript(std::uint64_t seed, std::size_t length) {
    ScriptRandom random(seed);
    std::vector<Command> commands;
    for (std::size_t i = 0; i < length; ++i) {
        const unsigned int kind = random.below(100);
        const OrderId id = random.below(80) + 1;
        const Side side = random.below(2) == 0 ? Side::BUY : Side::SELL;
        const Price price = 95 + random.below(11);
        const Quantity quantity = random.below(20) + 1;
        if (kind < 55) {
            commands.push_back(Submit{Order(id, price, quantity, 1, side)});
        } else if (kind < 70) {
            commands.push_back(Add{Order(id, price, quantity, 1, side)});
        } else if (kind < 95) {
            commands.push_back(Cancel{random.below(85)});  // includes 0 and IDs never used
        } else if (kind < 98) {
            commands.push_back(Submit{zeroPriced(id)});
        } else {
            commands.push_back(Add{zeroQuantity(id)});
        }
    }
    return commands;
}

// Follows each order's open quantity through the events alone and checks it against the
// book: an order is accepted with its quantity, each trade takes the same amount from both
// orders, and a cancellation reports exactly what was left. A duplicate names an ID that is
// resting, and a refused cancellation one that is not.
class Ledger {
public:
    void apply(const Outcome& outcome, const std::string& context) {
        for (const BookEvent& event : outcome.events) {
            if (const auto* accepted = std::get_if<OrderAccepted>(&event)) {
                expect(open.find(accepted->orderId) == open.end(), context + ": an accepted ID should not be resting");
                open[accepted->orderId] = accepted->quantity;
            } else if (const auto* trade = std::get_if<TradeExecuted>(&event)) {
                take(trade->restingOrderId, trade->executionQuantity, context);
                take(trade->incomingOrderId, trade->executionQuantity, context);
                ++trades;
            } else if (const auto* cancelled = std::get_if<OrderCancelled>(&event)) {
                const auto itr = open.find(cancelled->orderId);
                expect(itr != open.end() && itr->second == cancelled->cancelledQuantity,
                       context + ": a cancellation should report exactly what the order had left");
                open.erase(itr);
                ++cancellations;
            } else if (const auto* rejected = std::get_if<OrderRejected>(&event)) {
                // Malformed fields are reported first, so only a duplicate says whether the ID rests.
                expect(rejected->reason != RejectionReason::DUPLICATE_ORDER_ID ||
                           open.find(rejected->orderId) != open.end(),
                       context + ": a submission should be refused as a duplicate only while its ID is resting");
                ++rejections;
            } else {
                const auto& refused = std::get<CancelRejected>(event);
                expect(open.find(refused.orderId) == open.end(),
                       context + ": a cancellation should be refused only for an ID that is not resting");
                ++rejections;
            }
        }
        if (!outcome.refusal.empty()) {
            ++refusedAdds;
        }
    }

    void expectMatches(const OrderBook& book, const std::string& context) const {
        expect(book.activeOrderCount() == open.size(), context + ": the book should rest exactly the open orders");
        for (const auto& [orderId, quantity] : open) {
            expect(isIndexed(book, orderId) && book.findActiveOrder(orderId).getRemainingQuantity() == quantity,
                   context + ": order " + std::to_string(orderId) + " should rest with its open quantity");
        }
    }

    std::size_t trades = 0;
    std::size_t cancellations = 0;
    std::size_t rejections = 0;
    std::size_t refusedAdds = 0;

private:
    void take(OrderId orderId, Quantity quantity, const std::string& context) {
        const auto itr = open.find(orderId);
        expect(itr != open.end() && itr->second >= quantity,
               context + ": a trade should take no more than either order had open");
        itr->second -= quantity;
        if (itr->second == 0) {
            open.erase(itr);
        }
    }

    std::map<OrderId, Quantity> open;
};

// The same long command sequence, run on fresh books, gives the same outcome for every
// command and the same final book and counters; and those outcomes account for every unit.
void testDeterministicReplay() {
    const std::string context = "deterministic replay";
    const std::vector<Command> script = randomScript(20261022, 3000);

    OrderBook book;
    Ledger ledger;
    std::vector<Outcome> first;
    for (std::size_t i = 0; i < script.size(); ++i) {
        const std::string step = context + " command " + std::to_string(i);
        first.push_back(run(book, script[i]));
        expectBookConsistent(book, step);
        ledger.apply(first.back(), step);
        ledger.expectMatches(book, step);
    }
    const BookState firstState = capture(book, false);
    expect(ledger.trades > 100 && ledger.cancellations > 50 && ledger.rejections > 50 && ledger.refusedAdds > 10,
           context + ": the script should exercise trades, cancellations, rejections, and refused adds");

    for (int rerun = 0; rerun < 2; ++rerun) {
        BookState state;
        const std::vector<Outcome> again = replay(script, state, context + " rerun");
        expect(again.size() == first.size(), context + ": every command should report an outcome");
        const auto mismatch = std::mismatch(first.begin(), first.end(), again.begin());
        expect(mismatch.first == first.end(),
               context + ": rerunning should give the same outcomes, but command " +
                   std::to_string(mismatch.first - first.begin()) + " differed");
        expect(state == firstState, context + ": rerunning should leave the same book and counters");
    }

    BookState otherState;
    expect(replay(randomScript(7, 3000), otherState, context + " other seed") != first,
           context + ": a different script should not happen to give the same outcomes");
}

}  // namespace

int main() {
    testAlternatingBuysAndSells();
    testAlternatingSidesAtOnePrice();
    testManyPriceLevels();
    testRepeatedCancellations();
    testSubmitAfterCancellation();
    for (Side side : {Side::BUY, Side::SELL}) {
        testManyOrdersAtOnePrice(side);
        testRepeatedPartialFills(side);
        testEmptyingOneSide(side);
    }
    testInvalidCommandsMixedWithValid();
    testCommandsNearNumericLimits();
    testDeterministicReplay();

    std::cout << "scenario tests passed" << std::endl;
    return EXIT_SUCCESS;
}
