#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <list>
#include <new>
#include <stdexcept>
#include <string>

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

void testCancelOrderFailsBeforeRemoving() {
    // Recording the cancellation event allocates, so it must happen before the order is removed.
    const long failures = failEachAllocationInTurn(
        [](OrderBook& book) {
            book.addOrder(Order(1, 100, 5, 1, Side::SELL));
            book.addOrder(Order(2, 100, 5, 2, Side::SELL));
        },
        [](OrderBook& book) { book.cancelOrder(1); },
        [](const OrderBook& book, const std::string& context) {
            expect(Access::asks(book).at(100).getAggregateQuantity() == 10 && Access::index(book).size() == 2,
                   context + ": cancelOrder should only fail before the order is removed");
        },
        "cancelOrder");
    expect(failures > 0, "cancelOrder: at least one allocation failure should have been injected");
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
        testConsumeLevelFailsBeforeFilling();
        testCancelOrderFailsBeforeRemoving();

        std::cout << "allocation failure tests passed" << std::endl;
        return EXIT_SUCCESS;
    }
}
