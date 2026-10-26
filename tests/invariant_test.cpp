#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <list>
#include <optional>
#include <string>

#include "order_book/invariants.hpp"
#include "order_book/order.hpp"
#include "order_book/order_book.hpp"

namespace order_book {

// Befriended by OrderBook, PriceLevel, and Order so these tests can break the book's
// structures on purpose, one at a time, and check that the invariant checker notices.
struct OrderBookTestAccess {
    static auto& index(OrderBook& book) { return book.orderIndex; }
    static auto& bids(OrderBook& book) { return book.bids; }
    static auto& asks(OrderBook& book) { return book.asks; }
    static SequenceNum nextSequenceNum(const OrderBook& book) { return book.nextSequenceNum; }
    static void setNextSequenceNum(OrderBook& book, SequenceNum next) { book.nextSequenceNum = next; }
    // Rests without validation or numbering, so it can rest an order that crosses.
    static void restOrder(OrderBook& book, const Order& order) { book.restOrder(order); }

    static PriceLevel& level(OrderBook& book, Side side, Price price) {
        return side == Side::BUY ? book.bids.at(price) : book.asks.at(price);
    }
    static std::list<Order>& orders(PriceLevel& level) { return level.orders; }
    static std::uint64_t& aggregate(PriceLevel& level) { return level.quantity; }

    // The resting order itself, through its index entry.
    static Order& indexedOrder(OrderBook& book, OrderId orderId) { return *book.orderIndex.at(orderId).itr; }
    static void setRemaining(Order& order, unsigned int quantity) { order.remainingQuant = quantity; }
    static void setSequenceNum(Order& order, SequenceNum sequenceNum) { order.sequenceNum = sequenceNum; }
    static void setSide(Order& order, Side side) { order.side = side; }
    static void setPrice(Order& order, Price price) { order.priceTick = price; }
};

}  // namespace order_book

namespace {

using Access = order_book::OrderBookTestAccess;
using order_book::FillStatus;
using order_book::InvariantResult;
using order_book::InvariantViolation;
using order_book::Order;
using order_book::OrderBook;
using order_book::OrderId;
using order_book::OrderLocation;
using order_book::Price;
using order_book::RejectionReason;
using order_book::SequenceNum;
using order_book::Side;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

void expectSound(const OrderBook& book, const std::string& context) {
    const InvariantResult result = book.checkInvariants();
    expect(result.isValid(), context + ": the book should pass its invariant check, got " +
                                 order_book::formatInvariantResult(result));
}

// The checker should report exactly `wanted` (every field but the detail, which should
// say something).
void expectViolation(const OrderBook& book, const InvariantResult& wanted, const std::string& context) {
    const InvariantResult result = book.checkInvariants();
    const std::string got = order_book::formatInvariantResult(result);
    expect(!result.isValid(), context + ": the checker should report a violation");
    expect(result.violation == wanted.violation,
           context + ": expected " + order_book::formatInvariantResult(wanted) + ", got " + got);
    expect(result.side == wanted.side && result.price == wanted.price && result.orderId == wanted.orderId,
           context + ": the violation should name the offending side, price, and order, got " + got);
    expect(result.expected == wanted.expected && result.actual == wanted.actual,
           context + ": the violation should report the expected and actual values, got " + got);
    expect(*result.detail != '\0', context + ": the violation should say which check failed");
}

// Bids at 99 (orders 1, 2) and 98 (order 3), asks at 101 (orders 4, 5) and 102 (order 6),
// numbered 1 to 6 in that order.
OrderBook restingBook() {
    OrderBook book;
    book.submitOrder(Order(1, 99, 5, 1, Side::BUY));
    book.submitOrder(Order(2, 99, 3, 1, Side::BUY));
    book.submitOrder(Order(3, 98, 4, 1, Side::BUY));
    book.submitOrder(Order(4, 101, 6, 1, Side::SELL));
    book.submitOrder(Order(5, 101, 2, 1, Side::SELL));
    book.submitOrder(Order(6, 102, 7, 1, Side::SELL));
    expectSound(book, "resting book");
    return book;
}

void testSoundBooksPass() {
    expectSound(OrderBook{}, "empty book");

    // Sweeps, partial fills, remainders, and cancellations on both sides, checked after each.
    OrderBook book = restingBook();
    book.submitOrder(Order(7, 101, 7, 1, Side::BUY));  // takes 4, then 1 of 5
    expectSound(book, "partial fill at the best ask");
    book.submitOrder(Order(8, 103, 9, 1, Side::BUY));  // takes the rest of 5 and all of 6, rests 1 at 103
    expectSound(book, "sweep that rests a bid remainder");
    book.submitOrder(Order(9, 97, 14, 1, Side::SELL));  // takes 8, then every bid, rests 1 at 97
    expectSound(book, "sweep that rests an ask remainder");
    book.addOrder(Order(10, 96, 2, 1, Side::BUY));
    book.addOrder(Order(11, 96, 2, 1, Side::BUY));
    expectSound(book, "added bids");
    expect(book.cancelOrder(10).isSuccess() && book.cancelOrder(9).isSuccess(), "cancellations should succeed");
    expectSound(book, "after cancellations");
    book.consumeLevel(Side::BUY, 96, 2);
    expectSound(book, "after consuming a level");
}

// Time priority is queue position, and numbers wrap from the maximum back to 1, so a level
// queued across a wrap is in order even though its numbers are not ascending.
void testSequenceWrapPasses() {
    const std::string context = "sequence wrap";
    constexpr SequenceNum maxSequence = std::numeric_limits<SequenceNum>::max();
    OrderBook book;
    Access::setNextSequenceNum(book, maxSequence - 1);
    book.addOrder(Order(1, 100, 1, 1, Side::SELL));
    book.addOrder(Order(2, 100, 1, 1, Side::SELL));
    book.addOrder(Order(3, 100, 1, 1, Side::SELL));
    expect(Access::indexedOrder(book, 3).getSequenceNum() == 1, context + ": the third order should be numbered 1");
    expectSound(book, context);

    // Queuing the wrapped order first is still out of order.
    std::list<Order>& queue = Access::orders(Access::level(book, Side::SELL, 100));
    queue.splice(queue.begin(), queue, std::prev(queue.end()));
    expectViolation(book,
                    {.violation = InvariantViolation::INVALID_FIFO_SEQUENCE, .side = Side::SELL, .price = 100,
                     .orderId = 1, .expected = 1, .actual = maxSequence - 1},
                    context + " reordered");
}

void testLevelChecks() {
    {
        OrderBook book = restingBook();
        Access::bids(book).try_emplace(97, 97, Side::BUY);
        expectViolation(book, {.violation = InvariantViolation::EMPTY_PRICE_LEVEL, .side = Side::BUY, .price = 97},
                        "empty bid level");
    }
    {
        OrderBook book = restingBook();
        Access::asks(book).try_emplace(103, 103, Side::SELL);
        expectViolation(book, {.violation = InvariantViolation::EMPTY_PRICE_LEVEL, .side = Side::SELL, .price = 103},
                        "empty ask level");
    }
    {
        OrderBook book = restingBook();
        Access::bids(book).try_emplace(97, 96, Side::BUY);
        expectViolation(book,
                        {.violation = InvariantViolation::MISPLACED_LEVEL, .side = Side::BUY, .price = 97,
                         .expected = 97, .actual = 96},
                        "level stored under another price");
    }
    {
        OrderBook book = restingBook();
        Access::asks(book).try_emplace(103, 103, Side::BUY);
        expectViolation(book, {.violation = InvariantViolation::MISPLACED_LEVEL, .side = Side::SELL, .price = 103},
                        "bid level stored among the asks");
    }
}

void testOrderChecks() {
    {
        OrderBook book = restingBook();
        Access::setRemaining(Access::indexedOrder(book, 2), 0);
        Access::aggregate(Access::level(book, Side::BUY, 99)) -= 3;
        expectViolation(book,
                        {.violation = InvariantViolation::ZERO_QUANTITY_ORDER, .side = Side::BUY, .price = 99,
                         .orderId = 2},
                        "zero-quantity resting order");
    }
    {
        OrderBook book = restingBook();
        Access::indexedOrder(book, 5).setExecutionLevel(FillStatus::FILLED);
        expectViolation(book,
                        {.violation = InvariantViolation::MALFORMED_ORDER, .side = Side::SELL, .price = 101,
                         .orderId = 5},
                        "resting order marked FILLED");
    }
    {
        OrderBook book = restingBook();
        Access::setSequenceNum(Access::indexedOrder(book, 3), 0);
        expectViolation(book,
                        {.violation = InvariantViolation::MALFORMED_ORDER, .side = Side::BUY, .price = 98,
                         .orderId = 3},
                        "resting order without a sequence number");
    }
    {
        OrderBook book = restingBook();
        Access::setSide(Access::indexedOrder(book, 1), static_cast<Side>(2));
        expectViolation(book,
                        {.violation = InvariantViolation::MALFORMED_ORDER, .side = Side::BUY, .price = 99,
                         .orderId = 1},
                        "resting order with an invalid side");
        expect(std::string(book.checkInvariants().detail) == order_book::rejectionMessage(RejectionReason::INVALID_SIDE),
               "a malformed order's detail should be the reason Order refuses it");
    }
    {
        OrderBook book = restingBook();
        Access::setSide(Access::indexedOrder(book, 6), Side::BUY);
        expectViolation(book,
                        {.violation = InvariantViolation::MISPLACED_ORDER, .side = Side::SELL, .price = 102,
                         .orderId = 6},
                        "buy order resting among the asks");
    }
    {
        OrderBook book = restingBook();
        Access::setPrice(Access::indexedOrder(book, 4), 105);
        expectViolation(book,
                        {.violation = InvariantViolation::MISPLACED_ORDER, .side = Side::SELL, .price = 101,
                         .orderId = 4, .expected = 101, .actual = 105},
                        "order resting at another price's level");
    }
    {
        // Moving the newest order to the front keeps every iterator valid but breaks time priority.
        OrderBook book = restingBook();
        std::list<Order>& queue = Access::orders(Access::level(book, Side::BUY, 99));
        queue.splice(queue.begin(), queue, std::prev(queue.end()));
        expectViolation(book,
                        {.violation = InvariantViolation::INVALID_FIFO_SEQUENCE, .side = Side::BUY, .price = 99,
                         .orderId = 1, .expected = 2, .actual = 1},
                        "level queued out of acceptance order");
    }
    {
        OrderBook book = restingBook();
        Access::aggregate(Access::level(book, Side::SELL, 102)) += 1;
        expectViolation(book,
                        {.violation = InvariantViolation::AGGREGATE_QUANTITY_MISMATCH, .side = Side::SELL,
                         .price = 102, .expected = 7, .actual = 8},
                        "cached aggregate out of step");
    }
}

void testIndexChecks() {
    {
        OrderBook book = restingBook();
        Access::index(book).erase(5);
        expectViolation(book,
                        {.violation = InvariantViolation::MISSING_INDEX_ENTRY, .side = Side::SELL, .price = 101,
                         .orderId = 5},
                        "resting order without an index entry");
    }
    {
        OrderBook book = restingBook();
        Access::index(book).at(3).side = Side::SELL;
        expectViolation(book,
                        {.violation = InvariantViolation::INVALID_INDEX_ENTRY, .side = Side::BUY, .price = 98,
                         .orderId = 3},
                        "index entry recording the wrong side");
    }
    {
        OrderBook book = restingBook();
        Access::index(book).at(3).tickPrice = 97;
        expectViolation(book,
                        {.violation = InvariantViolation::INVALID_INDEX_ENTRY, .side = Side::BUY, .price = 98,
                         .orderId = 3, .expected = 98, .actual = 97},
                        "index entry recording the wrong price");
    }
    {
        // The right side, price, and list position, but a handle to another level.
        OrderBook book = restingBook();
        Access::index(book).at(1).level = Access::index(book).at(3).level;
        expectViolation(book,
                        {.violation = InvariantViolation::INVALID_INDEX_ENTRY, .side = Side::BUY, .price = 99,
                         .orderId = 1},
                        "index entry pointing at another level");
        expect(std::string(book.checkInvariants().detail) == "an index entry should point at its order's level",
               "index entry pointing at another level: the violation should name the level check");
        Access::index(book).at(1).level = nullptr;
        expectViolation(book,
                        {.violation = InvariantViolation::INVALID_INDEX_ENTRY, .side = Side::BUY, .price = 99,
                         .orderId = 1},
                        "index entry with no level");
    }
    {
        OrderBook book = restingBook();
        Access::index(book).at(1).itr = Access::index(book).at(2).itr;
        expectViolation(book,
                        {.violation = InvariantViolation::INVALID_INDEX_ENTRY, .side = Side::BUY, .price = 99,
                         .orderId = 1},
                        "index entry pointing at another order in its level");
    }
    {
        // Removing an order without unindexing it, the way a cancel or fill could get wrong.
        // The entry's iterator now dangles, so the checker must find it by key alone.
        OrderBook book = restingBook();
        auto& level = Access::level(book, Side::SELL, 101);
        Access::aggregate(level) -= 2;
        Access::orders(level).erase(Access::index(book).at(5).itr);
        expectViolation(book,
                        {.violation = InvariantViolation::INVALID_INDEX_ENTRY, .side = Side::SELL, .price = 101,
                         .orderId = 5},
                        "index entry left behind by a removed order");
    }
    {
        // The same, but the removed order was its level's last, so the level went too.
        OrderBook book = restingBook();
        Access::orders(Access::level(book, Side::SELL, 102)).erase(Access::index(book).at(6).itr);
        Access::asks(book).erase(102);
        expectViolation(book,
                        {.violation = InvariantViolation::INVALID_INDEX_ENTRY, .side = Side::SELL, .price = 102,
                         .orderId = 6},
                        "index entry left behind by an erased level");
    }
    {
        OrderBook book = restingBook();
        const OrderLocation location = Access::index(book).at(1);
        Access::index(book).emplace(77, location);
        expectViolation(book,
                        {.violation = InvariantViolation::INVALID_INDEX_ENTRY, .side = Side::BUY, .price = 99,
                         .orderId = 77},
                        "index entry for an ID that is not resting");
    }
    {
        OrderBook book = restingBook();
        const OrderLocation location = Access::index(book).at(1);
        Access::index(book).emplace(78, OrderLocation(99, static_cast<Side>(2), location.itr, location.level));
        expectViolation(book,
                        {.violation = InvariantViolation::INVALID_INDEX_ENTRY, .price = 99, .orderId = 78},
                        "index entry with an invalid side");
    }
    {
        // A second order with ID 3 queued behind the first; the index can point at only one.
        OrderBook book = restingBook();
        const SequenceNum next = Access::nextSequenceNum(book);
        auto& level = Access::level(book, Side::BUY, 98);
        Access::orders(level).push_back(Order(3, 98, 4, next, Side::BUY));
        Access::aggregate(level) += 4;
        Access::setNextSequenceNum(book, next + 1);
        expectViolation(book,
                        {.violation = InvariantViolation::DUPLICATE_ORDER_ID, .side = Side::BUY, .price = 98,
                         .orderId = 3},
                        "ID resting twice in one level");
    }
    {
        // ID 1 rests as a bid; a copy of the ID resting as an ask is just as much a duplicate.
        OrderBook book = restingBook();
        const SequenceNum next = Access::nextSequenceNum(book);
        auto& level = Access::level(book, Side::SELL, 102);
        Access::orders(level).push_back(Order(1, 102, 2, next, Side::SELL));
        Access::aggregate(level) += 2;
        Access::setNextSequenceNum(book, next + 1);
        expectViolation(book,
                        {.violation = InvariantViolation::DUPLICATE_ORDER_ID, .side = Side::SELL, .price = 102,
                         .orderId = 1},
                        "ID resting on both sides");
    }
}

void testCrossedBook() {
    for (const Price bidPrice : {Price{101}, Price{102}}) {
        const std::string context = "bid resting at " + std::to_string(bidPrice) + " against an ask at 101";
        OrderBook book = restingBook();
        const SequenceNum next = Access::nextSequenceNum(book);
        Access::restOrder(book, Order(7, bidPrice, 1, next, Side::BUY));
        Access::setNextSequenceNum(book, next + 1);
        expectViolation(book, {.violation = InvariantViolation::CROSSED_BOOK, .expected = 101, .actual = bidPrice},
                        context);
    }
}

// Checks run bids, then asks, then the index, then the top of the book, stopping at the
// first failure, so a broken ask aggregate is reported ahead of a missing bid entry.
void testStopsAtFirstViolation() {
    OrderBook book = restingBook();
    Access::index(book).erase(1);
    Access::aggregate(Access::level(book, Side::SELL, 101)) += 1;
    expectViolation(book,
                    {.violation = InvariantViolation::AGGREGATE_QUANTITY_MISMATCH, .side = Side::SELL, .price = 101,
                     .expected = 8, .actual = 9},
                    "two violations");
}

void testFormat() {
    expect(order_book::formatInvariantResult(InvariantResult{}) == "valid", "a sound result should format as valid");

    OrderBook book = restingBook();
    Access::aggregate(Access::level(book, Side::SELL, 102)) += 1;
    expect(order_book::formatInvariantResult(book.checkInvariants()) ==
               "AGGREGATE_QUANTITY_MISMATCH side=SELL price=102 expected=7 actual=8"
               " (a level's cached quantity should equal its orders' remaining sum)",
           "a violation should format as its name, each field present, and its detail");

    OrderBook crossed = restingBook();
    Access::restOrder(crossed, Order(7, 102, 1, Access::nextSequenceNum(crossed), Side::BUY));
    expect(order_book::formatInvariantResult(crossed.checkInvariants()) ==
               "CROSSED_BOOK expected=101 actual=102 (the best bid should be strictly below the best ask)",
           "fields that do not apply should be left out");
}

}  // namespace

int main() {
    testSoundBooksPass();
    testSequenceWrapPasses();
    testLevelChecks();
    testOrderChecks();
    testIndexChecks();
    testCrossedBook();
    testStopsAtFirstViolation();
    testFormat();

    std::cout << "invariant tests passed" << std::endl;
    return EXIT_SUCCESS;
}
