#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <list>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "order_book/order.hpp"
#include "order_book/order_book.hpp"

// Fault injection: every allocation in this executable goes through this operator new,
// which can be armed to throw std::bad_alloc on the Nth allocation from now.
namespace {
long allocationsBeforeFailure = -1;  // negative = disarmed

// MSVC's checked iterators allocate bookkeeping proxies inside noexcept container members
// (e.g. vector's default constructor), so failing one calls std::terminate. The test only
// runs without them, e.g. in a Release build; otherwise CTest reports it as skipped.
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
constexpr bool kIteratorDebugging = true;
#else
constexpr bool kIteratorDebugging = false;
#endif
constexpr int kSkippedExitCode = 77;  // matches SKIP_RETURN_CODE in CMakeLists.txt
}

void* operator new(std::size_t size) {
    if (allocationsBeforeFailure == 0) {
        allocationsBeforeFailure = -1;
        throw std::bad_alloc();
    }
    if (allocationsBeforeFailure > 0) {
        --allocationsBeforeFailure;
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace order_book {

// Befriended by OrderBook and PriceLevel so these tests can inspect internals.
struct OrderBookTestAccess {
    static const auto& index(const OrderBook& book) { return book.orderIndex; }
    static const auto& bids(const OrderBook& book) { return book.bids; }
    static const auto& asks(const OrderBook& book) { return book.asks; }
    static TradeId nextTradeId(const OrderBook& book) { return book.nextTradeId; }
    static SequenceNum nextSequenceNum(const OrderBook& book) { return book.nextSequenceNum; }

    static const std::list<Order>& orders(const PriceLevel& level) { return level.orders; }
};

}  // namespace order_book

namespace {

using Access = order_book::OrderBookTestAccess;
using order_book::Order;
using order_book::OrderBook;
using order_book::Side;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
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

// Runs `operation` on a freshly set-up book with its Nth allocation failing, for
// N = 0, 1, 2, ... until it completes; `afterFailure` checks the book after each throw.
// Returns how many failures were injected, so callers can confirm the test is not vacuous.
template <typename Setup, typename Operation, typename AfterFailure>
long failEachAllocationInTurn(Setup setup, Operation operation, AfterFailure afterFailure,
                              const std::string& context) {
    for (long failAt = 0; failAt < 1000; ++failAt) {
        OrderBook book{};
        setup(book);

        bool threw = false;
        allocationsBeforeFailure = failAt;
        try {
            operation(book);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        allocationsBeforeFailure = -1;

        const std::string attempt = context + " (allocation " + std::to_string(failAt) + " failed)";
        expectIndexConsistent(book, attempt);
        if (!threw) {
            return failAt;
        }
        afterFailure(book, attempt);
    }
    expect(false, context + ": the operation never completed");
    return 0;
}

void testAddOrderToNewLevelRollsBack() {
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) { book.addOrder(Order(1, 100, 5, 1, Side::SELL)); },
        [](OrderBook& book) { book.addOrder(Order(2, 101, 5, 2, Side::SELL)); },
        [](const OrderBook& book, const std::string& context) {
            expect(Access::asks(book).size() == 1, context + ": a failed add should not leave a new level behind");
            expect(Access::index(book).size() == 1, context + ": a failed add should not leave an index entry");
            expect(Access::nextSequenceNum(book) == 2, context + ": a failed add should not consume a sequence number");
        },
        "addOrder (new level)");
    expect(failures > 0, "addOrder (new level): at least one allocation failure should have been injected");
}

void testAddOrderToExistingLevelRollsBack() {
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) { book.addOrder(Order(1, 100, 5, 1, Side::SELL)); },
        [](OrderBook& book) { book.addOrder(Order(2, 100, 7, 2, Side::SELL)); },
        [](const OrderBook& book, const std::string& context) {
            const auto& level = Access::asks(book).at(100);
            expect(level.getOrdersQuantity() == 1 && level.getAggregateQuantity() == 5,
                   context + ": a failed add should leave the existing level as it was");
            expect(Access::index(book).size() == 1, context + ": a failed add should not leave an index entry");
            expect(Access::nextSequenceNum(book) == 2, context + ": a failed add should not consume a sequence number");
        },
        "addOrder (existing level)");
    expect(failures > 0, "addOrder (existing level): at least one allocation failure should have been injected");
}

// A submission that does not cross rests whole without matching. Whatever allocation fails,
// the book is left as it was: no index entry, no order, no new or emptied level. Its sequence
// number is consumed, since submitOrder numbers an order as soon as it is accepted and every
// allocation comes after that.
void expectRestingSubmitRollsBack(Order incoming, const std::string& context) {
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) { book.addOrder(Order(1, 100, 5, 1, Side::SELL)); },
        [incoming](OrderBook& book) { book.submitOrder(incoming); },
        [](const OrderBook& book, const std::string& attempt) {
            const auto& asks = Access::asks(book);
            expect(asks.size() == 1 && asks.at(100).getOrdersQuantity() == 1 && asks.at(100).getAggregateQuantity() == 5,
                   attempt + ": a failed resting submission should leave the asks as they were");
            expect(Access::bids(book).empty(), attempt + ": a failed resting submission should not leave a bid level");
            expect(Access::index(book).size() == 1 && Access::index(book).count(2) == 0,
                   attempt + ": a failed resting submission should not leave an index entry");
            expect(Access::nextSequenceNum(book) == 3,
                   attempt + ": a resting submission accepted before failing should still have consumed its number");
        },
        context);
    expect(failures > 0, context + ": at least one allocation failure should have been injected");
}

void testRestingSubmitRollsBack() {
    expectRestingSubmitRollsBack(Order(2, 100, 5, 9, Side::SELL), "submitOrder resting (existing level)");
    expectRestingSubmitRollsBack(Order(2, 99, 5, 9, Side::BUY), "submitOrder resting (new level)");
}

void testSubmitOrderStaysConsistent() {
    // Sweeps two asks, then rests the remainder as a new bid level.
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) {
            book.addOrder(Order(1, 100, 2, 1, Side::SELL));
            book.addOrder(Order(3, 101, 2, 2, Side::SELL));
        },
        [](OrderBook& book) { book.submitOrder(Order(2, 101, 6, 3, Side::BUY)); },
        [](const OrderBook& book, const std::string& context) {
            // Executions before the failure may stand, but the remainder must not be half-rested.
            expect(!book.hasBids() && Access::index(book).find(2) == Access::index(book).end(),
                   context + ": a failed submission should not rest the incoming order");
            const unsigned int executions = Access::nextTradeId(book) - 1;
            expect(Access::index(book).size() == 2 - executions,
                   context + ": every execution made should have filled exactly one resting order");
            expect(Access::nextSequenceNum(book) == 4,
                   context + ": a submission accepted before failing should still have consumed its number");
        },
        "submitOrder");
    expect(failures > 0, "submitOrder: at least one allocation failure should have been injected");
}

// A sweep of six trades outgrows the result's inline storage: the trades vector grows after
// four executions already stand (matchOrder makes room before each execution, so a failure
// there happens before the fifth), and the seven events spill to the heap when the result is
// built. Whatever fails, every execution made is a filled resting order, and the remainder
// never rests.
void testLongSweepStaysConsistent() {
    std::vector<long> injected;
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) {
            for (unsigned int i = 0; i < 6; ++i) {
                book.addOrder(Order(10 + i, 100 + i, 1, 1, Side::SELL));
            }
        },
        [](OrderBook& book) { book.submitOrder(Order(2, 106, 7, 1, Side::BUY)); },
        [&injected](const OrderBook& book, const std::string& context) {
            expect(!book.hasBids() && Access::index(book).count(2) == 0,
                   context + ": a failed sweep should not rest the incoming order");
            const unsigned int executions = Access::nextTradeId(book) - 1;
            expect(Access::index(book).size() == 6 - executions && Access::asks(book).size() == 6 - executions,
                   context + ": every execution made should have filled exactly one resting order and its level");
            expect(Access::nextSequenceNum(book) == 8,
                   context + ": a submission accepted before failing should still have consumed its number");
            injected.push_back(static_cast<long>(executions));
        },
        "submitOrder (six-trade sweep)");
    expect(failures > 0, "submitOrder (six-trade sweep): at least one allocation failure should have been injected");
    expect(!injected.empty() && injected.front() == 4,
           "the first allocation of a six-trade sweep should be the trades vector growing after four executions");
}

void testConsumeLevelFailsBeforeFilling() {
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) {
            book.addOrder(Order(1, 100, 5, 1, Side::SELL));
            book.addOrder(Order(2, 100, 5, 2, Side::SELL));
        },
        [](OrderBook& book) { book.consumeLevel(Side::SELL, 100, 7); },
        [](const OrderBook& book, const std::string& context) {
            expect(Access::asks(book).at(100).getAggregateQuantity() == 10 && Access::index(book).size() == 2,
                   context + ": consumeLevel should only fail before any order is filled");
        },
        "consumeLevel");
    expect(failures > 0, "consumeLevel: at least one allocation failure should have been injected");
}

void testCancelOrderDoesNotAllocate() {
    // A cancellation's result holds its one event inline, and removing an order only frees,
    // so cancelOrder allocates nothing: with the very first allocation armed to fail, both a
    // successful and a refused cancellation complete, and the failure is never triggered.
    OrderBook book{};
    book.addOrder(Order(1, 100, 5, 1, Side::SELL));
    book.addOrder(Order(2, 100, 5, 2, Side::SELL));

    allocationsBeforeFailure = 0;
    bool threw = false;
    bool cancelled = false;
    bool refused = false;
    try {
        cancelled = book.cancelOrder(1).isSuccess();
        refused = !book.cancelOrder(99).isSuccess();
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    const bool stillArmed = allocationsBeforeFailure == 0;
    allocationsBeforeFailure = -1;

    expect(!threw && stillArmed, "cancelOrder should not allocate");
    expect(cancelled && refused, "cancelOrder should cancel a resting order and refuse an unknown one");
    expect(Access::asks(book).at(100).getAggregateQuantity() == 5 && Access::index(book).size() == 1,
           "the cancelled order should be gone and the other left resting");
    expectIndexConsistent(book, "cancelOrder without allocating");
}

}  // namespace

int main() {
    if constexpr (kIteratorDebugging) {
        std::cout << "allocation failure tests skipped: MSVC iterator debugging is on" << std::endl;
        return kSkippedExitCode;
    } else {
        testAddOrderToNewLevelRollsBack();
        testAddOrderToExistingLevelRollsBack();
        testSubmitOrderStaysConsistent();
        testLongSweepStaysConsistent();
        testRestingSubmitRollsBack();
        testConsumeLevelFailsBeforeFilling();
        testCancelOrderDoesNotAllocate();

        std::cout << "allocation failure tests passed" << std::endl;
        return EXIT_SUCCESS;
    }
}
