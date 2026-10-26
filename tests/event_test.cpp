#include <cstdlib>
#include <iostream>
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

// Befriended by Order so these tests can build a malformed order the constructor would refuse.
struct OrderBookTestAccess {
    static void setPrice(Order& order, Price price) { order.priceTick = price; }
};

}  // namespace order_book

namespace {

using order_book::BookEvent;
using order_book::FillStatus;
using order_book::CancelRejected;
using order_book::Order;
using order_book::OrderAccepted;
using order_book::OrderBook;
using order_book::OrderCancelled;
using order_book::OrderRejected;
using order_book::RejectionReason;
using order_book::Side;
using order_book::TradeExecuted;

using Events = std::vector<BookEvent>;

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

std::string sideName(Side side) {
    return side == Side::BUY ? "buy" : "sell";
}

Side opposite(Side side) {
    return side == Side::BUY ? Side::SELL : Side::BUY;
}

// Resting liquidity sits at 100 and further from the incoming side (asks rise, bids fall),
// so an incoming limit at priceAway(side, n) crosses every resting level up to n.
order_book::Price priceAway(Side incomingSide, int ticksAway) {
    return static_cast<order_book::Price>(incomingSide == Side::BUY ? 100 + ticksAway : 100 - ticksAway);
}

// Orders 1 (3) and 2 (4) at 100, then 3 (5) one tick further away, then 4 (6) back at 100:
// price priority puts 4 ahead of 3, and time priority puts it behind 1 and 2.
OrderBook seededBook(Side incomingSide) {
    const Side restingSide = opposite(incomingSide);
    OrderBook book;
    book.submitOrder(Order(1, priceAway(incomingSide, 0), 3, 1, restingSide));
    book.submitOrder(Order(2, priceAway(incomingSide, 0), 4, 1, restingSide));
    book.submitOrder(Order(3, priceAway(incomingSide, 1), 5, 1, restingSide));
    book.submitOrder(Order(4, priceAway(incomingSide, 0), 6, 1, restingSide));
    return book;
}

void testDuplicateSubmissionEmitsOnlyOrderRejected() {
    OrderBook book;
    book.submitOrder(Order(1, 100, 5, 1, Side::BUY));

    const Events events = book.submitOrder(Order(1, 101, 7, 1, Side::SELL)).getEvents().toVector();
    expect(events == Events{OrderRejected{1, RejectionReason::DUPLICATE_ORDER_ID}},
           "a duplicate submission should emit exactly one OrderRejected with its ID and reason");
}

// Malformed and inactive orders are refused with the reason validation gives, and nothing else.
void testMalformedAndInactiveSubmissionsEmitOnlyOrderRejected() {
    OrderBook book;

    Order zeroPrice(2, 100, 5, 1, Side::BUY);
    order_book::OrderBookTestAccess::setPrice(zeroPrice, 0);
    expect(book.submitOrder(zeroPrice).getEvents() == Events{OrderRejected{2, RejectionReason::INVALID_PRICE}},
           "a zero-price submission should emit exactly one OrderRejected with INVALID_PRICE");

    Order filled(3, 100, 5, 1, Side::SELL);
    filled.setExecutionLevel(FillStatus::FILLED);
    expect(book.submitOrder(filled).getEvents() == Events{OrderRejected{3, RejectionReason::ORDER_NOT_ACTIVE}},
           "a submission marked FILLED should emit exactly one OrderRejected with ORDER_NOT_ACTIVE");
}

// Resting is not an event of its own: an accepted order that does not cross reports only
// its acceptance.
void testRestingSubmissionEmitsOnlyOrderAccepted(Side side) {
    const std::string context = sideName(side) + " resting submission";
    OrderBook book;

    const Events events = book.submitOrder(Order(4, 100, 9, 1, side)).getEvents().toVector();
    expect(events == Events{OrderAccepted{4, side, 100, 9}},
           context + ": should emit exactly one OrderAccepted with its ID, side, limit price, and quantity");
}

// One TradeExecuted per resting order matched, in matching order, each at the resting price;
// the last match takes only what the incoming order still needs.
void testFullyFilledSweepEmitsAcceptanceThenEachTrade(Side side) {
    const std::string context = sideName(side) + " fully filled sweep";
    OrderBook book = seededBook(side);

    const Events events = book.submitOrder(Order(9, priceAway(side, 1), 16, 1, side)).getEvents().toVector();
    const Events expected{
        OrderAccepted{9, side, priceAway(side, 1), 16},
        TradeExecuted{9, 1, priceAway(side, 0), 3},
        TradeExecuted{9, 2, priceAway(side, 0), 4},
        TradeExecuted{9, 4, priceAway(side, 0), 6},
        TradeExecuted{9, 3, priceAway(side, 1), 3},
    };
    expect(events == expected,
           context + ": should emit the acceptance, then one trade per match in price-time order");
}

// A remainder rests without an event of its own, and the acceptance still reports the
// quantity as submitted, not what was left after matching.
void testPartiallyFilledSubmissionEmitsNoRestingEvent(Side side) {
    const std::string context = sideName(side) + " partially filled submission";
    OrderBook book = seededBook(side);

    const Events events = book.submitOrder(Order(9, priceAway(side, 0), 20, 1, side)).getEvents().toVector();
    const Events expected{
        OrderAccepted{9, side, priceAway(side, 0), 20},
        TradeExecuted{9, 1, priceAway(side, 0), 3},
        TradeExecuted{9, 2, priceAway(side, 0), 4},
        TradeExecuted{9, 4, priceAway(side, 0), 6},
    };
    expect(events == expected,
           context + ": should emit the acceptance and its trades, with no event for the resting remainder");
    expect(book.findActiveOrder(9).getRemainingQuantity() == 7, context + ": the remainder should still rest");
}

// Adding rests without matching, so it reports only its acceptance, and it refuses an order
// that would cross by throwing, so it emits nothing for one.
void testAddOrderEmitsOnlyOrderAccepted() {
    OrderBook book;
    expect(book.addOrder(Order(1, 100, 5, 1, Side::SELL)).getEvents() == Events{OrderAccepted{1, Side::SELL, 100, 5}},
           "adding an order should emit exactly one OrderAccepted with its ID, side, price, and quantity");
    expect(book.addOrder(Order(2, 99, 3, 1, Side::BUY)).getEvents() == Events{OrderAccepted{2, Side::BUY, 99, 3}},
           "adding an order on the other side should emit only its OrderAccepted");
    expectThrows([&book] { book.addOrder(Order(3, 100, 1, 1, Side::BUY)); },
                 "adding an order that would cross should throw rather than emit anything");
}

// Cancelling reports what was still resting, not what was originally submitted.
void testCancellingPartiallyFilledOrderEmitsRemainingQuantity(Side side) {
    const std::string context = sideName(side) + " cancel after partial fill";
    OrderBook book;
    book.submitOrder(Order(5, 100, 100, 1, side));
    book.submitOrder(Order(6, 100, 70, 1, opposite(side)));

    const order_book::CancellationResult result = book.cancelOrder(5);
    expect(result.isSuccess(), context + ": the cancellation should succeed");
    expect(result.getEvents() == Events{OrderCancelled{5, 30}},
           context + ": should emit exactly one OrderCancelled with the 30 units left of 100");
}

void testCancellingUntouchedOrderEmitsFullQuantity() {
    OrderBook book;
    book.submitOrder(Order(5, 100, 12, 1, Side::SELL));

    expect(book.cancelOrder(5).getEvents() == Events{OrderCancelled{5, 12}},
           "cancelling an unfilled order should emit one OrderCancelled with its whole quantity");
}

// Unknown, already cancelled, and fully filled IDs are all refused the same way.
void testRefusedCancellationsEmitOnlyCancelRejected() {
    OrderBook book;
    book.submitOrder(Order(5, 100, 12, 1, Side::SELL));
    book.submitOrder(Order(6, 100, 4, 1, Side::BUY));
    book.submitOrder(Order(7, 100, 8, 1, Side::BUY));  // fills the rest of 5

    const Events unknown = book.cancelOrder(42).getEvents().toVector();
    expect(unknown == Events{CancelRejected{42, RejectionReason::ORDER_NOT_FOUND}},
           "cancelling an unknown ID should emit exactly one CancelRejected with ORDER_NOT_FOUND");

    const Events filled = book.cancelOrder(5).getEvents().toVector();
    expect(filled == Events{CancelRejected{5, RejectionReason::ORDER_NOT_FOUND}},
           "cancelling a fully filled order should emit exactly one CancelRejected");

    book.submitOrder(Order(8, 101, 3, 1, Side::SELL));
    book.cancelOrder(8);
    const Events repeated = book.cancelOrder(8).getEvents().toVector();
    expect(repeated == Events{CancelRejected{8, RejectionReason::ORDER_NOT_FOUND}},
           "cancelling an already cancelled order should emit exactly one CancelRejected");
}

// An order added straight to the book, without matching.
struct Add {
    Order order;
};

// A command is a submission, an add, or a cancellation by ID.
using Command = std::variant<Order, Add, order_book::OrderId>;

// Runs commands in order on a fresh book, concatenating each command's events.
Events replay(const std::vector<Command>& commands) {
    OrderBook book;
    Events stream;
    for (const Command& command : commands) {
        Events events;
        if (std::holds_alternative<Order>(command)) {
            events = book.submitOrder(std::get<Order>(command)).getEvents().toVector();
        } else if (std::holds_alternative<Add>(command)) {
            events = book.addOrder(std::get<Add>(command).order).getEvents().toVector();
        } else {
            events = book.cancelOrder(std::get<order_book::OrderId>(command)).getEvents().toVector();
        }
        stream.insert(stream.end(), events.begin(), events.end());
    }
    return stream;
}

// The same commands from the same starting state give the same event stream, and that stream
// is the one worked out by hand from the matching rules.
void testScriptedReplayIsDeterministic() {
    const std::vector<Command> script{
        Order(1, 101, 5, 1, Side::SELL),
        Order(2, 100, 3, 1, Side::SELL),
        Order(3, 99, 4, 1, Side::BUY),
        Add{Order(6, 98, 2, 1, Side::BUY)},
        Order(2, 99, 1, 1, Side::BUY),    // 2 is still resting
        Order(4, 101, 6, 1, Side::BUY),   // takes all of 2, then 3 of 1
        order_book::OrderId{1},           // 2 of 1 left
        order_book::OrderId{2},           // already filled
        Order(5, 98, 10, 1, Side::SELL),  // takes all of 3, then all of 6, rests 4
        order_book::OrderId{5},
    };
    const Events expected{
        OrderAccepted{1, Side::SELL, 101, 5},
        OrderAccepted{2, Side::SELL, 100, 3},
        OrderAccepted{3, Side::BUY, 99, 4},
        OrderAccepted{6, Side::BUY, 98, 2},
        OrderRejected{2, RejectionReason::DUPLICATE_ORDER_ID},
        OrderAccepted{4, Side::BUY, 101, 6},
        TradeExecuted{4, 2, 100, 3},
        TradeExecuted{4, 1, 101, 3},
        OrderCancelled{1, 2},
        CancelRejected{2, RejectionReason::ORDER_NOT_FOUND},
        OrderAccepted{5, Side::SELL, 98, 10},
        TradeExecuted{5, 3, 99, 4},
        TradeExecuted{5, 6, 98, 2},
        OrderCancelled{5, 4},
    };

    const Events first = replay(script);
    expect(first == expected, "the scripted replay should emit the hand-worked event stream");
    expect(replay(script) == first, "replaying the same script on a fresh book should emit the same stream");
}

}  // namespace

int main() {
    testDuplicateSubmissionEmitsOnlyOrderRejected();
    testMalformedAndInactiveSubmissionsEmitOnlyOrderRejected();
    for (Side side : {Side::BUY, Side::SELL}) {
        testRestingSubmissionEmitsOnlyOrderAccepted(side);
        testFullyFilledSweepEmitsAcceptanceThenEachTrade(side);
        testPartiallyFilledSubmissionEmitsNoRestingEvent(side);
        testCancellingPartiallyFilledOrderEmitsRemainingQuantity(side);
    }
    testAddOrderEmitsOnlyOrderAccepted();
    testCancellingUntouchedOrderEmitsFullQuantity();
    testRefusedCancellationsEmitOnlyCancelRejected();
    testScriptedReplayIsDeterministic();

    std::cout << "event tests passed" << std::endl;
    return EXIT_SUCCESS;
}
